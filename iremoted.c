/*
 * iremoted.c
 * Display events received from the Apple Infrared Remote.
 *
 * gcc -Wall -o iremoted iremoted.c -framework IOKit -framework Carbon
 *
 * Copyright (c) 2006-2008 Amit Singh. All Rights Reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *     
 *  THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 *  OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 *  SUCH DAMAGE.
 */

#define PROGNAME "iremoted"
#define PROGVERS "2.0"

#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/errno.h>
#include <sysexits.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Carbon/Carbon.h>

static struct option
long_options[] = {
    { "help",    no_argument, 0, 'h' },
    { "keynote", no_argument, 0, 'k' },
    { 0, 0, 0, 0 },
};

static const char *options = "hk";

IOHIDElementCookie buttonNextID = 0;
IOHIDElementCookie buttonPreviousID = 0;

typedef struct cookie_struct
{
    IOHIDElementCookie gButtonCookie_SystemAppMenu;
    IOHIDElementCookie gButtonCookie_SystemMenuSelect;
    IOHIDElementCookie gButtonCookie_SystemMenuRight;
    IOHIDElementCookie gButtonCookie_SystemMenuLeft;
    IOHIDElementCookie gButtonCookie_SystemMenuUp;
    IOHIDElementCookie gButtonCookie_SystemMenuDown;
} *cookie_struct_t;

enum {
    keynoteEventClass = 'Kntc',
    slideForward      = 'steF',
    slideBackward     = 'steB',
};

static const char *keynoteID = "com.apple.iWork.Keynote";
static int driveKeynote = 0;

void            usage(void);
OSStatus        KeynoteChangeSlide(AEEventID eventID);
inline          void print_errmsg_if_io_err(int expr, char *msg);
inline          void print_errmsg_if_err(int expr, char *msg);
void            QueueCallbackFunction(void *target, IOReturn result,
                                      void *refcon, void *sender);
bool            addQueueCallbacks(IOHIDQueueInterface **hqi);
void            processQueue(IOHIDDeviceInterface **hidDeviceInterface,
                             cookie_struct_t cookies);
void            doRun(IOHIDDeviceInterface **hidDeviceInterface,
                      cookie_struct_t cookies);
cookie_struct_t getHIDCookies(IOHIDDeviceInterface122 **handle);
void            createHIDDeviceInterface(io_object_t hidDevice,
                                         IOHIDDeviceInterface ***hdi);
void            setupAndRun(void);

void
usage(void)
{
    printf("%s (version %s)\n", PROGNAME, PROGVERS);
    printf("Copyright (c) 2006-2008 Amit Singh. All Rights Reserved.\n");
    printf("Displays events received from the Apple Infrared Remote.\n");
    printf("Usage: %s [OPTIONS...]\n\nOptions:\n", PROGNAME);
    printf("  -h, --help    print this help message and exit\n");
    printf("  -k, --keynote use forward/backward button presses for Keynote slide transition\n\n");
    printf("Please report bugs using the following contact information:\n"
           "<URL:http://www.osxbook.com/software/bugs/>\n");
}

OSStatus
KeynoteChangeSlide(AEEventID eventID)
{
    OSStatus     err = noErr;
    AppleEvent   eventToSend = { typeNull, nil };
    AppleEvent   eventReply  = { typeNull, nil };
    AEBuildError eventBuildError;

    err = AEBuildAppleEvent(
              keynoteEventClass,     // Event class for the resulting event
              eventID,               // Event ID for the resulting event
              typeApplicationBundleID,
              keynoteID,
              strlen(keynoteID),
              kAutoGenerateReturnID, // Return ID for the created event
              kAnyTransactionID,     // Transaction ID for this event
              &eventToSend,          // Pointer to location for storing result
              &eventBuildError,      // Pointer to error structure
              "",                    // AEBuild format string describing the
              NULL                   // AppleEvent record to be created
        );
    if (err != noErr) {
        fprintf(stderr, "Failed to build Apple event (error %d).\n", (int)err);
        return err;
    }

    err = AESend(&eventToSend,
                 &eventReply,
                 kAEWaitReply,      // send mode (wait for reply)
                 kAENormalPriority,
                 kNoTimeOut,
                 nil,               // no pointer to idle function
                 nil);              // no pointer to filter function
    
    if (err != noErr)
        fprintf(stderr, "Failed to send Apple event (error %d).\n", (int)err);

    // Dispose of the send/reply descs
    AEDisposeDesc(&eventToSend);
    AEDisposeDesc(&eventReply);

    return err;
}

inline void
print_errmsg_if_io_err(int expr, char *msg)
{
    IOReturn err = (expr);

    if (err != kIOReturnSuccess) {
        fprintf(stderr, "*** %s - %s(%x, %d).\n", msg, mach_error_string(err),
                err, err & 0xffffff);
        fflush(stderr);
        exit(EX_OSERR);
    }
}

inline void
print_errmsg_if_err(int expr, char *msg)
{
    if (expr) {
        fprintf(stderr, "*** %s.\n", msg);
        fflush(stderr);
        exit(EX_OSERR);
    }
}

void
QueueCallbackFunction(void *target, IOReturn result, void *refcon, void *sender)
{
    HRESULT               ret = 0;
    AbsoluteTime          zeroTime = {0,0};
    IOHIDQueueInterface **hqi;
    IOHIDEventStruct      event;

    while (!ret) {
        hqi = (IOHIDQueueInterface **)sender;
        ret = (*hqi)->getNextEvent(hqi, &event, zeroTime, 0);
        if (!ret) {
            printf("%#lx %s\n", (UInt32)event.elementCookie,
                   (event.value == 0) ? "depressed" : "pressed");
            fflush(stdout);
            if (event.value && driveKeynote) {
                if (event.elementCookie == buttonNextID)
                    KeynoteChangeSlide(slideForward);
                else if (event.elementCookie == buttonPreviousID)
                    KeynoteChangeSlide(slideBackward);
            }
        }
    }
}

bool
addQueueCallbacks(IOHIDQueueInterface **hqi)
{
    IOReturn               ret;
    CFRunLoopSourceRef     eventSource;
    IOHIDQueueInterface ***privateData;

    privateData = malloc(sizeof(*privateData));
    *privateData = hqi;

    ret = (*hqi)->createAsyncEventSource(hqi, &eventSource);
    if (ret != kIOReturnSuccess)
        return false;

    ret = (*hqi)->setEventCallout(hqi, QueueCallbackFunction,
                                  NULL, &privateData);
    if (ret != kIOReturnSuccess)
        return false;

    CFRunLoopAddSource(CFRunLoopGetCurrent(), eventSource,
                       kCFRunLoopDefaultMode);
    return true;
}

void
processQueue(IOHIDDeviceInterface **hidDeviceInterface, cookie_struct_t cookies)
{
    HRESULT               result;
    IOHIDQueueInterface **queue;

    queue = (*hidDeviceInterface)->allocQueue(hidDeviceInterface);
    if (!queue) {
        fprintf(stderr, "Failed to allocate event queue.\n");
        return;
    }

    (void)(*queue)->create(queue, 0, 8);

    (void)(*queue)->addElement(queue,
                               cookies->gButtonCookie_SystemAppMenu, 0);

    (void)(*queue)->addElement(queue,
                               cookies->gButtonCookie_SystemMenuSelect, 0);

    (void)(*queue)->addElement(queue,
                               cookies->gButtonCookie_SystemMenuRight, 0);

    (void)(*queue)->addElement(queue,
                               cookies->gButtonCookie_SystemMenuLeft, 0);

    (void)(*queue)->addElement(queue,
                               cookies->gButtonCookie_SystemMenuUp, 0);

    (void)(*queue)->addElement(queue,
                               cookies->gButtonCookie_SystemMenuDown, 0);

    addQueueCallbacks(queue);

    result = (*queue)->start(queue);
    
    CFRunLoopRun();

    result = (*queue)->stop(queue);

    result = (*queue)->dispose(queue);

    (*queue)->Release(queue);
}

void
doRun(IOHIDDeviceInterface **hidDeviceInterface, cookie_struct_t cookies)
{
    IOReturn ioReturnValue;

    ioReturnValue = (*hidDeviceInterface)->open(hidDeviceInterface, 0);

    processQueue(hidDeviceInterface, cookies);

    if (ioReturnValue == KERN_SUCCESS)
        ioReturnValue = (*hidDeviceInterface)->close(hidDeviceInterface);
    (*hidDeviceInterface)->Release(hidDeviceInterface);
}

cookie_struct_t
getHIDCookies(IOHIDDeviceInterface122 **handle)
{
    cookie_struct_t    cookies;
    IOHIDElementCookie cookie;
    CFTypeRef          object;
    long               number;
    long               usage;
    long               usagePage;
    CFArrayRef         elements;
    CFDictionaryRef    element;
    IOReturn           result;

    if ((cookies = (cookie_struct_t)malloc(sizeof(*cookies))) == NULL) {
        fprintf(stderr, "Failed to allocate cookie memory.\n");
        exit(1);
    }

    memset(cookies, 0, sizeof(*cookies));

    if (!handle || !(*handle))
        return cookies;

    result = (*handle)->copyMatchingElements(handle, NULL, &elements);

    if (result != kIOReturnSuccess) {
        fprintf(stderr, "Failed to copy cookies.\n");
        exit(1);
    }

    CFIndex i;
    for (i = 0; i < CFArrayGetCount(elements); i++) {
        element = CFArrayGetValueAtIndex(elements, i);
        object = (CFDictionaryGetValue(element, CFSTR(kIOHIDElementCookieKey)));
        if (object == 0 || CFGetTypeID(object) != CFNumberGetTypeID())
            continue;
        if(!CFNumberGetValue((CFNumberRef) object, kCFNumberLongType, &number))
            continue;
        cookie = (IOHIDElementCookie)number;
        object = CFDictionaryGetValue(element, CFSTR(kIOHIDElementUsageKey));
        if (object == 0 || CFGetTypeID(object) != CFNumberGetTypeID())
            continue;
        if (!CFNumberGetValue((CFNumberRef)object, kCFNumberLongType, &number))
            continue;
        usage = number;
        object = CFDictionaryGetValue(element,CFSTR(kIOHIDElementUsagePageKey));
        if (object == 0 || CFGetTypeID(object) != CFNumberGetTypeID())
            continue;
        if (!CFNumberGetValue((CFNumberRef)object, kCFNumberLongType, &number))
            continue;
        usagePage = number;

        if (usagePage == kHIDPage_GenericDesktop) {
            switch (usage) {
            case kHIDUsage_GD_SystemAppMenu:
                cookies->gButtonCookie_SystemAppMenu = cookie;
                break;
            case kHIDUsage_GD_SystemMenu:
                cookies->gButtonCookie_SystemMenuSelect = cookie;
                break;
            case kHIDUsage_GD_SystemMenuRight:
                buttonNextID = cookie;
                cookies->gButtonCookie_SystemMenuRight = cookie;
                break;
            case kHIDUsage_GD_SystemMenuLeft:
                buttonPreviousID = cookie;
                cookies->gButtonCookie_SystemMenuLeft = cookie;
                break;
            case kHIDUsage_GD_SystemMenuUp:
                cookies->gButtonCookie_SystemMenuUp = cookie;
                break;
            case kHIDUsage_GD_SystemMenuDown:
                cookies->gButtonCookie_SystemMenuDown = cookie;
                break;
            }
        }
    }

    return cookies;
}

void
createHIDDeviceInterface(io_object_t hidDevice, IOHIDDeviceInterface ***hdi)
{
    io_name_t             className;
    IOCFPlugInInterface **plugInInterface = NULL;
    HRESULT               plugInResult = S_OK;
    SInt32                score = 0;
    IOReturn              ioReturnValue = kIOReturnSuccess;

    ioReturnValue = IOObjectGetClass(hidDevice, className);

    print_errmsg_if_io_err(ioReturnValue, "Failed to get class name.");

    ioReturnValue = IOCreatePlugInInterfaceForService(
                        hidDevice,
                        kIOHIDDeviceUserClientTypeID,
                        kIOCFPlugInInterfaceID,
                        &plugInInterface,
                        &score);

    if (ioReturnValue != kIOReturnSuccess)
        return;

    plugInResult = (*plugInInterface)->QueryInterface(
                        plugInInterface,
                        CFUUIDGetUUIDBytes(kIOHIDDeviceInterfaceID),
                        (LPVOID)hdi);
    print_errmsg_if_err(plugInResult != S_OK,
                        "Failed to create device interface.\n");

    (*plugInInterface)->Release(plugInInterface);
}

void
setupAndRun(void)
{
    CFMutableDictionaryRef hidMatchDictionary = NULL;
    io_service_t           hidService = (io_service_t)0;
    io_object_t            hidDevice = (io_object_t)0;
    IOHIDDeviceInterface **hidDeviceInterface = NULL;
    IOReturn               ioReturnValue = kIOReturnSuccess;
    cookie_struct_t        cookies;
    
    hidMatchDictionary = IOServiceNameMatching("AppleIRController");
    hidService = IOServiceGetMatchingService(kIOMasterPortDefault,
                                             hidMatchDictionary);

    if (!hidService) {
        fprintf(stderr, "Apple Infrared Remote not found.\n");
        exit(1);
    }

    hidDevice = (io_object_t)hidService;

    createHIDDeviceInterface(hidDevice, &hidDeviceInterface);
    cookies = getHIDCookies((IOHIDDeviceInterface122 **)hidDeviceInterface);
    ioReturnValue = IOObjectRelease(hidDevice);
    print_errmsg_if_io_err(ioReturnValue, "Failed to release HID.");

    if (hidDeviceInterface == NULL) {
        fprintf(stderr, "No HID.\n");
        exit(1);
    }

    ioReturnValue = (*hidDeviceInterface)->open(hidDeviceInterface, 0);

    doRun(hidDeviceInterface, cookies);

    if (ioReturnValue == KERN_SUCCESS)
        ioReturnValue = (*hidDeviceInterface)->close(hidDeviceInterface);

    (*hidDeviceInterface)->Release(hidDeviceInterface);
}

int
main (int argc, char **argv)
{
    int c, option_index = 0;

    while ((c = getopt_long(argc, argv, options, long_options, &option_index))
         != -1) {
        switch (c) {
        case 'h':
            usage();
            exit(0);
            break;
        case 'k':
            driveKeynote = 1;
            break;
        default:
            usage();
            exit(1);
            break;
        }
    }

    setupAndRun();

    return 0;
}
