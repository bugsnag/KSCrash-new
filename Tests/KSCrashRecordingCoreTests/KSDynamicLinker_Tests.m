//
//  KSDynamicLinker_Tests.m
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

#import <XCTest/XCTest.h>
#include <mach-o/dyld.h>

#import "KSDynamicLinker.h"

const struct mach_header header1 = {
    .magic = MH_MAGIC,
    .cputype = 0,
    .cpusubtype = 0,
    .filetype = 0,
    .ncmds = 1,
    .sizeofcmds = 0,
    .flags = 0
};
const struct segment_command command1 = {
    .cmd = LC_SEGMENT,
    .cmdsize = 0,
    .segname = SEG_TEXT,
    .vmaddr = 111,
    .vmsize = 10,
};

const struct mach_header header2 = {
    .magic = MH_MAGIC,
    .cputype = 0,
    .cpusubtype = 0,
    .filetype = 0,
    .ncmds = 1,
    .sizeofcmds = 0,
    .flags = 0
};
const struct segment_command command2 = {
    .cmd = LC_SEGMENT,
    .cmdsize = 0,
    .segname = SEG_TEXT,
    .vmaddr = 222,
    .vmsize = 10,
};

@interface KSDynamicLinker_Tests : XCTestCase
@end

@implementation KSDynamicLinker_Tests

+ (void)setUp {
    [super setUp];
    ksdl_binary_images_initialize();
}

static KSBinaryImage *get_tail(KSBinaryImage *head) {
    KSBinaryImage *current = head;
    for (; current->next != NULL; current = current->next) {
    }
    return current;
}

- (void)testImageUUID
{
    // Just abritrarily grab the name of the 4th image...
    const char *name = _dyld_get_image_name(4);
    const uint8_t *uuidBytes = ksdl_imageUUID(name, true);
    XCTAssertTrue(uuidBytes != NULL, @"");
}

- (void)testImageUUIDInvalidName
{
    const uint8_t *uuidBytes = ksdl_imageUUID("sdfgserghwerghwrh", true);
    XCTAssertTrue(uuidBytes == NULL, @"");
}

- (void)testImageUUIDNULLName
{
    const uint8_t *uuidBytes = ksdl_imageUUID(NULL, true);
    XCTAssertTrue(uuidBytes == NULL, @"");
}

- (void)testImageUUIDPartialMatch
{
    const uint8_t *uuidBytes = ksdl_imageUUID("libSystem", false);
    XCTAssertTrue(uuidBytes != NULL, @"");
}

- (void)testGetImageNameNULL
{
    KSBinaryImage *image = ksdl_imageNamed(NULL, false);
    XCTAssertEqual(image, NULL, @"");
}

- (void)testAddRemove {
    ksdl_test_support_mach_headers_reset();

    ksdl_test_support_mach_headers_add_image(&header1, 0);

    KSBinaryImage *listTail = get_tail(ksdl_get_images());
    XCTAssertEqual(listTail->vmAddress, command1.vmaddr);
    XCTAssert(listTail->unloaded == FALSE);

    ksdl_test_support_mach_headers_add_image(&header2, 0);

    XCTAssertEqual(listTail->vmAddress, command1.vmaddr);
    XCTAssert(listTail->unloaded == FALSE);
    XCTAssertEqual(listTail->next->vmAddress, command2.vmaddr);
    XCTAssert(listTail->next->unloaded == FALSE);

    ksdl_test_support_mach_headers_remove_image(&header1, 0);

    XCTAssertEqual(listTail->vmAddress, command1.vmaddr);
    XCTAssert(listTail->unloaded == TRUE);
    XCTAssertEqual(listTail->next->vmAddress, command2.vmaddr);
    XCTAssert(listTail->next->unloaded == FALSE);

    ksdl_test_support_mach_headers_remove_image(&header2, 0);

    XCTAssertEqual(listTail->vmAddress, command1.vmaddr);
    XCTAssert(listTail->unloaded == TRUE);
    XCTAssertEqual(listTail->next->vmAddress, command2.vmaddr);
    XCTAssert(listTail->next->unloaded == TRUE);
}

- (void)testFindImageAtAddress {
    ksdl_test_support_mach_headers_reset();

    ksdl_test_support_mach_headers_add_image(&header1, 0);
    ksdl_test_support_mach_headers_add_image(&header2, 0);

    KSBinaryImage *item;
    item = ksdl_image_at_address((uintptr_t)&header1);
    XCTAssertEqual(item->vmAddress, command1.vmaddr);

    item = ksdl_image_at_address((uintptr_t)&header2);
    XCTAssertEqual(item->vmAddress, command2.vmaddr);
}

- (void)testGetSelfImage {
    ksdl_binary_images_initialize();

    NSString *nameStr =  [NSString stringWithUTF8String:ksdl_get_self_image()->name];
    XCTAssertNotEqual([nameStr rangeOfString:@"KSCrashRecordingCoreTests"].location, NSNotFound);
}

- (void)testMainImage {
    XCTAssertEqualObjects(@(ksdl_get_main_image()->name),
                          NSBundle.mainBundle.executablePath);
}

- (void)testImageAtAddress {
    for (NSNumber *number in NSThread.callStackReturnAddresses) {
        uintptr_t address = number.unsignedIntegerValue;
        KSBinaryImage *image = ksdl_image_at_address(address);
        struct dl_info dlinfo = {0};
        if (dladdr((const void*)address, &dlinfo) != 0) {
            // If dladdr was able to locate the image, so should bsg_mach_headers_image_at_address
            XCTAssertEqual(image->header, dlinfo.dli_fbase);
            XCTAssertEqual(image->vmAddress + image->slide, (uint64_t)dlinfo.dli_fbase);
            XCTAssertEqual(image->name, dlinfo.dli_fname);
            XCTAssertFalse(image->unloaded);
        }
    }

    XCTAssertEqual(ksdl_image_at_address(0x0000000000000000), NULL);
    XCTAssertEqual(ksdl_image_at_address(0x0000000000001000), NULL);
    XCTAssertEqual(ksdl_image_at_address(0x7FFFFFFFFFFFFFFF), NULL);
}

@end
