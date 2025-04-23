//
//  KSDynamicLinker.h
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

#ifndef HDR_KSDynamicLinker_h
#define HDR_KSDynamicLinker_h

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ks_mach_image {
    /// The mach_header or mach_header_64
    ///
    /// This is also the memory address where the __TEXT segment has been loaded by dyld, including slide.
    const struct mach_header *header;

    /// The vmaddr specified for the __TEXT segment
    ///
    /// This is the load address specified at build time, and does not account for slide applied by dyld.
    uint64_t vmAddress;

    /// The vmsize of the __TEXT segment
    uint64_t size;

    /// The pathname of the shared object (Dl_info.dli_fname)
    const char *name;

    /// A UUID that uniquely identifies this image, used to identify its associated dSYM
    const uint8_t *uuid;

    /// The virtual memory address slide of the image
    intptr_t slide;

    /// True if the image has been unloaded and should be ignored
    bool unloaded;

    /// True if the image is referenced by the current crash report.
    bool inCrashReport;

    int cpuType;
    int cpuSubType;
    uint64_t majorVersion;
    uint64_t minorVersion;
    uint64_t revisionVersion;
    const char *crashInfoMessage;
    const char *crashInfoMessage2;
    const char *crashInfoBacktrace;
    const char *crashInfoSignature;
    
    /// The next image in the linked list
    _Atomic(struct ks_mach_image *) next;
} KSBinaryImage;

/**
 * Initialize the headers management system.
 * This MUST be called before calling anything else.
 */
void ksdl_binary_images_initialize(void);

/**
 * Returns the head of the link list of Binary Image info
 */
KSBinaryImage *ksdl_get_images(void);

/** Get information about a binary image based on mach_header.
 *
 * @param header The Mach binary image header.
 *
 * @param image_name The name of the image.
 *
 * @param buffer A structure to hold the information.
 *
 * @return True if the image was successfully queried.
 */
bool ksdl_getBinaryImageForHeader(const struct mach_header *header, intptr_t slide, KSBinaryImage *buffer);

/** Find a loaded binary image with the specified name.
 *
 * @param imageName The image name to look for.
 *
 * @param exactMatch If true, look for an exact match instead of a partial one.
 *
 * @return the matched image, or NULL if not found.
 */
KSBinaryImage *ksdl_imageNamed(const char *const imageName, bool exactMatch);

/** Get the UUID of a loaded binary image with the specified name.
 *
 * @param imageName The image name to look for.
 *
 * @param exactMatch If true, look for an exact match instead of a partial one.
 *
 * @return A pointer to the binary (16 byte) UUID of the image, or NULL if it
 *         wasn't found.
 */
const uint8_t *ksdl_imageUUID(const char *const imageName, bool exactMatch);

/**
 * Returns the process's main image
 */
KSBinaryImage *ksdl_get_main_image(void);

/**
 * Returns the image that contains KSCrash.
 */
KSBinaryImage *ksdl_get_self_image(void);

/**
 * Find the loaded binary image that contains the specified instruction address.
*/
KSBinaryImage *ksdl_image_at_address(const uintptr_t address);

/** async-safe version of dladdr.
 *
 * This method searches the dynamic loader for information about any image
 * containing the specified address. It may not be entirely successful in
 * finding information, in which case any fields it could not find will be set
 * to NULL.
 *
 * Unlike dladdr(), this method does not make use of locks, and does not call
 * async-unsafe functions.
 *
 * @param address The address to search for.
 * @param info Gets filled out by this function.
 * @return true if at least some information was found.
 */
bool ksdl_dladdr(const uintptr_t address, Dl_info *const info);

/**
 * Resets mach header data (for unit tests).
 */
void ksdl_test_support_mach_headers_reset(void);

/**
 * Add a binary image (for unit tests).
 */
void ksdl_test_support_mach_headers_add_image(const struct mach_header *mh, intptr_t slide);

/**
 * Remove a binary image (for unit tests).
 */
void ksdl_test_support_mach_headers_remove_image(const struct mach_header *mh, intptr_t slide);

#ifdef __cplusplus
}
#endif

#endif  // HDR_KSDynamicLinker_h
