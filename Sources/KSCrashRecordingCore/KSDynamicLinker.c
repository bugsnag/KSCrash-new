//
//  KSDynamicLinker.c
//
//  Created by Karl Stenerud on 2013-10-02.
//
//  Copyright (c) 2012 Karl Stenerud. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall remain in place
// in this source code.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "KSDynamicLinker.h"

#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
#include <mach-o/getsect.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#include <os/trace.h>
#include <string.h>
#include <stdlib.h>

#include "KSLogger.h"
#include "KSMemory.h"
#include "KSPlatformSpecificDefines.h"

#ifndef KSDL_MaxCrashInfoStringLength
#define KSDL_MaxCrashInfoStringLength 4096
#endif

#pragma pack(8)
typedef struct {
    unsigned version;
    const char *message;
    const char *signature;
    const char *backtrace;
    const char *message2;
    void *reserved;
    void *reserved2;
    void *reserved3;  // First introduced in version 5
} crash_info_t;
#pragma pack()
#define KSDL_SECT_CRASH_INFO "__crash_info"

#pragma mark - Declarations -

static void register_dyld_images(void);
static void register_for_changes(void);
static void add_image(const struct mach_header *header, intptr_t slide);
static void remove_image(const struct mach_header *header, intptr_t slide);
static intptr_t compute_slide(const struct mach_header *header);
static const char * get_path(const struct mach_header *header);

static const struct dyld_all_image_infos *g_all_image_infos;

#pragma mark - Binary images linked list -

// The list head is implemented as a dummy entry to simplify the algorithm.
// We fetch g_head_dummy.next to get the real head of the list.
static KSBinaryImage g_head_dummy;
static _Atomic(KSBinaryImage *) g_images_tail = &g_head_dummy;
static KSBinaryImage *g_self_image;

static _Atomic(bool) is_image_list_initialized;

void ksdl_binary_images_initialize(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&is_image_list_initialized, &expected, true)) {
        // Already called
        return;
    }

    register_dyld_images();
    register_for_changes();
}

static void register_dyld_images(void) {
    // /usr/lib/dyld's mach header is is not exposed via the _dyld APIs, so to be able to include information
    // about stack frames in dyld`start (for example) we need to acess "_dyld_all_image_infos"
    task_dyld_info_data_t dyld_info = {0};
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), TASK_DYLD_INFO, (task_info_t)&dyld_info, &count);
    if (kr == KERN_SUCCESS && dyld_info.all_image_info_addr) {
        g_all_image_infos = (const void *)dyld_info.all_image_info_addr;

        intptr_t dyldImageSlide = compute_slide(g_all_image_infos->dyldImageLoadAddress);
        add_image(g_all_image_infos->dyldImageLoadAddress, dyldImageSlide);

#if TARGET_OS_SIMULATOR
        // Get the mach header for `dyld_sim` which is not exposed via the _dyld APIs
        // Note: dladdr() returns `/usr/lib/dyld` as the dli_fname for this image :-?
        if (g_all_image_infos->infoArray &&
            strstr(g_all_image_infos->infoArray->imageFilePath, "/usr/lib/dyld_sim")) {
            const struct mach_header *header = g_all_image_infos->infoArray->imageLoadAddress;
            add_image(header, compute_slide(header));
        }
#endif
    } else {
        KSLOG_ERROR("task_info TASK_DYLD_INFO failed: %s", mach_error_string(kr));
    }
}

static intptr_t compute_slide(const struct mach_header *header) {
    uintptr_t cmdPtr = ksdl_first_cmd_after_header(header);
    if (!cmdPtr) {
        return 0;
    }
    for (uint32_t iCmd = 0; iCmd < header->ncmds; iCmd++) {
        struct load_command *loadCmd = (void *)cmdPtr;
        switch (loadCmd->cmd) {
            case LC_SEGMENT: {
                struct segment_command *segCmd = (void *)cmdPtr;
                if (strcmp(segCmd->segname, SEG_TEXT) == 0) {
                    return (intptr_t)header - (intptr_t)segCmd->vmaddr;
                }
            }
            case LC_SEGMENT_64: {
                struct segment_command_64 *segCmd = (void *)cmdPtr;
                if (strcmp(segCmd->segname, SEG_TEXT) == 0) {
                    return (intptr_t)header - (intptr_t)segCmd->vmaddr;
                }
            }
        }
        cmdPtr += loadCmd->cmdsize;
    }
    return 0;
}

static void register_for_changes(void) {
    // Register for binary images being loaded and unloaded. dyld calls the add function once
    // for each library that has already been loaded and then keeps this cache up-to-date
    // with future changes
    _dyld_register_func_for_add_image(&add_image);
    _dyld_register_func_for_remove_image(&remove_image);
}

static void add_image(const struct mach_header *header, intptr_t slide) {
    KSBinaryImage *newImage = calloc(1, sizeof(KSBinaryImage));
    if (newImage == NULL) {
        return;
    }

    if (!ksdl_getBinaryImageForHeader(header, slide, newImage)) {
        free(newImage);
        return;
    }

    KSBinaryImage *oldTail = atomic_exchange(&g_images_tail, newImage);
    atomic_store(&oldTail->next, newImage);

    if (header == &__dso_handle) {
        g_self_image = newImage;
    }
}

static void remove_image(const struct mach_header *header, intptr_t slide) {
    KSBinaryImage existingImage = { 0 };
    if (!ksdl_getBinaryImageForHeader(header, slide, &existingImage)) {
        return;
    }

    for (KSBinaryImage *img = ksdl_get_images(); img != NULL; img = atomic_load(&img->next)) {
        if (img->vmAddress == existingImage.vmAddress) {
            // To avoid a destructive operation that could lead thread safety problems,
            // we maintain the image record, but mark it as unloaded
            img->unloaded = true;
        }
    }
}

#pragma mark - API -

static const char * get_path(const struct mach_header *header) {
    Dl_info DlInfo = {0};
    dladdr(header, &DlInfo);
    if (DlInfo.dli_fname) {
        return DlInfo.dli_fname;
    }
    if (g_all_image_infos &&
        header == g_all_image_infos->dyldImageLoadAddress) {
        return g_all_image_infos->dyldPath;
    }
#if TARGET_OS_SIMULATOR
    if (g_all_image_infos &&
        g_all_image_infos->infoArray &&
        header == g_all_image_infos->infoArray[0].imageLoadAddress) {
        return g_all_image_infos->infoArray[0].imageFilePath;
    }
#endif
    return NULL;
}

uintptr_t ksdl_first_cmd_after_header(const struct mach_header * header)
{
    if (header == NULL) {
      return 0;
    }

    switch (header->magic) {
        case MH_MAGIC:
        case MH_CIGAM:
            return (uintptr_t)(header + 1);
        case MH_MAGIC_64:
        case MH_CIGAM_64:
            return (uintptr_t)(((struct mach_header_64 *)header) + 1);
        default:
            // Header is corrupt
            return 0;
    }
}

KSBinaryImage *ksdl_get_images(void) {
    return atomic_load(&g_head_dummy.next);
}

KSBinaryImage *ksdl_imageNamed(const char *const imageName, bool exactMatch)
{
    if (imageName != NULL) {
        for (KSBinaryImage *img = ksdl_get_images(); img != NULL; img = atomic_load(&img->next)) {
            if (img->name == NULL) {
                continue; // name is null if the index is out of range per dyld(3)
            } else if (img->unloaded == true) {
                continue; // ignore unloaded libraries
            } else if (exactMatch) {
                if (strcmp(img->name, imageName) == 0) {
                    return img;
                }
            } else {
                if (strstr(img->name, imageName) != NULL) {
                    return img;
                }
            }
        }
    }

    return NULL;
}

const uint8_t *ksdl_imageUUID(const char *const imageName, bool exactMatch)
{
    if (imageName != NULL) {
        KSBinaryImage *img = ksdl_imageNamed(imageName, exactMatch);
        if (img != NULL) {
            if (img->header != NULL) {
                uintptr_t cmdPtr = ksdl_first_cmd_after_header(img->header);
                if (cmdPtr != 0) {
                    for (uint32_t iCmd = 0; iCmd < img->header->ncmds; iCmd++) {
                        const struct load_command *loadCmd = (struct load_command *)cmdPtr;
                        if (loadCmd->cmd == LC_UUID) {
                            struct uuid_command *uuidCmd = (struct uuid_command *)cmdPtr;
                            return uuidCmd->uuid;
                        }
                        cmdPtr += loadCmd->cmdsize;
                    }
                }
            }
        }
    }
    return NULL;
}

KSBinaryImage *ksdl_get_main_image(void) {
    for (KSBinaryImage *img = ksdl_get_images(); img != NULL; img = atomic_load(&img->next)) {
        if (img->header->filetype == MH_EXECUTE) {
            return img;
        }
    }
    return NULL;
}

KSBinaryImage *ksdl_get_self_image(void) {
    return g_self_image;
}

static bool contains_address(KSBinaryImage *img, vm_address_t address) {
    if (img->unloaded) {
        return false;
    }
    vm_address_t imageStart = (vm_address_t)img->header;
    return address >= imageStart && address < (imageStart + img->size);
}

KSBinaryImage *ksdl_image_at_address(const uintptr_t address){
    for (KSBinaryImage *img = ksdl_get_images(); img; img = atomic_load(&img->next)) {
        if (contains_address(img, address)) {
            return img;
        }
    }
    return NULL;
}

static bool isValidCrashInfoMessage(const char *str)
{
    if (str == NULL) {
        return false;
    }
    int maxReadableBytes = ksmem_maxReadableBytes(str, KSDL_MaxCrashInfoStringLength + 1);
    if (maxReadableBytes == 0) {
        return false;
    }
    for (int i = 0; i < maxReadableBytes; ++i) {
        if (str[i] == 0) {
            return true;
        }
    }
    return false;
}

static void getCrashInfo(KSBinaryImage *buffer)
{
    unsigned long size = 0;
    crash_info_t *crashInfo =
        (crash_info_t *)getsectiondata((mach_header_t *)buffer->header, SEG_DATA, KSDL_SECT_CRASH_INFO, &size);
    if (crashInfo == NULL) {
        return;
    }

    KSLOG_TRACE("Found crash info section in binary: %s", buffer->name);
    const unsigned int minimalSize = offsetof(crash_info_t, reserved);  // Include message and message2
    if (size < minimalSize) {
        KSLOG_TRACE("Skipped reading crash info: section is too small");
        return;
    }
    if (!ksmem_isMemoryReadable(crashInfo, minimalSize)) {
        KSLOG_TRACE("Skipped reading crash info: section memory is not readable");
        return;
    }
    if (crashInfo->version != 4 && crashInfo->version != 5) {
        KSLOG_TRACE("Skipped reading crash info: invalid version '%d'", crashInfo->version);
        return;
    }
    if (crashInfo->message == NULL && crashInfo->message2 == NULL) {
        KSLOG_TRACE("Skipped reading crash info: both messages are null");
        return;
    }

    if (isValidCrashInfoMessage(crashInfo->message)) {
        KSLOG_DEBUG("Found first message: %s", crashInfo->message);
        buffer->crashInfoMessage = crashInfo->message;
    }
    if (isValidCrashInfoMessage(crashInfo->message2)) {
        KSLOG_DEBUG("Found second message: %s", crashInfo->message2);
        buffer->crashInfoMessage2 = crashInfo->message2;
    }
    if (isValidCrashInfoMessage(crashInfo->backtrace)) {
        KSLOG_DEBUG("Found backtrace: %s", crashInfo->backtrace);
        buffer->crashInfoBacktrace = crashInfo->backtrace;
    }
    if (isValidCrashInfoMessage(crashInfo->signature)) {
        KSLOG_DEBUG("Found signature: %s", crashInfo->signature);
        buffer->crashInfoSignature = crashInfo->signature;
    }
}

bool ksdl_getBinaryImageForHeader(const struct mach_header *header, intptr_t slide, KSBinaryImage *buffer)
{
    // Early exit conditions; this is not a valid/useful binary image
    // 1. We can't find a sensible Mach command
    uintptr_t cmdPtr = ksdl_first_cmd_after_header(header);
    if (cmdPtr == 0) {
        return false;
    }

    // 2. The image doesn't have a name.  Note: running with a debugger attached causes this condition to match.
    const char *imageName = get_path(header);
    if (!imageName) {
        KSLOG_ERROR("Could not find name for mach header @ %p", header);
        return false;
    }

    // Look for the TEXT segment to get the image size.
    // Also look for a UUID command.
    uint64_t imageSize = 0;
    uint64_t imageVmAddr = 0;
    uint64_t version = 0;
    uint8_t *uuid = NULL;

    for (uint32_t iCmd = 0; iCmd < header->ncmds; iCmd++) {
        struct load_command *loadCmd = (struct load_command *)cmdPtr;
        switch (loadCmd->cmd) {
            case LC_SEGMENT: {
                struct segment_command *segCmd = (struct segment_command *)cmdPtr;
                if (strcmp(segCmd->segname, SEG_TEXT) == 0) {
                    imageSize = segCmd->vmsize;
                    imageVmAddr = segCmd->vmaddr;
                }
                break;
            }
            case LC_SEGMENT_64: {
                struct segment_command_64 *segCmd = (struct segment_command_64 *)cmdPtr;
                if (strcmp(segCmd->segname, SEG_TEXT) == 0) {
                    imageSize = segCmd->vmsize;
                    imageVmAddr = segCmd->vmaddr;
                }
                break;
            }
            case LC_UUID: {
                struct uuid_command *uuidCmd = (struct uuid_command *)cmdPtr;
                uuid = uuidCmd->uuid;
                break;
            }
            case LC_ID_DYLIB: {
                struct dylib_command *dc = (struct dylib_command *)cmdPtr;
                version = dc->dylib.current_version;
                break;
            }
        }
        cmdPtr += loadCmd->cmdsize;
    }

    // Sanity checks that should never fail
    if (((uintptr_t)imageVmAddr + (uintptr_t)slide) != (uintptr_t)header) {
        KSLOG_ERROR("Mach header != (vmaddr + slide) for %s; symbolication will be compromised.", imageName);
    }

    buffer->header = header;
    buffer->vmAddress = imageVmAddr;
    buffer->size = imageSize;
    buffer->name = imageName;
    buffer->uuid = uuid;
    buffer->slide = slide;
    buffer->unloaded = FALSE;
    buffer->cpuType = header->cputype;
    buffer->cpuSubType = header->cpusubtype;
    buffer->majorVersion = version >> 16;
    buffer->minorVersion = (version >> 8) & 0xff;
    buffer->revisionVersion = version & 0xff;
    getCrashInfo(buffer);
    atomic_store(&buffer->next, NULL);

    return true;
}

void ksdl_test_support_mach_headers_reset(void) {
    // Erase all current images
    KSBinaryImage *next = NULL;
    for (KSBinaryImage *img = ksdl_get_images(); img != NULL; img = next) {
        next = atomic_load(&img->next);
        free(img);
    }

    // Reset cached data
    atomic_store(&g_head_dummy.next, NULL);
    atomic_store(&g_images_tail, &g_head_dummy);
    g_self_image = NULL;

    // Force bsg_mach_headers_initialize to run again when requested.
    atomic_store(&is_image_list_initialized, false);
}

void ksdl_test_support_mach_headers_add_image(const struct mach_header *header, intptr_t slide) {
    add_image(header, slide);
}

void ksdl_test_support_mach_headers_remove_image(const struct mach_header *header, intptr_t slide) {
    remove_image(header, slide);
}
