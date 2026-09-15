/* Own-window fixture for test_desktop_live_mcp.py. No screenshots, keychain
 * operations or permission requests. An owned stdin shutdown command restores
 * the previous application if this fixture still owns keyboard focus. SIGTERM
 * is best effort because AppKit may terminate before its timer callback. */
#import <Cocoa/Cocoa.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;
static void request_stop(int sig) { (void)sig; stop_requested = 1; }

static void emit(NSDictionary *value) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];
    if (data) {
        fwrite(data.bytes, 1, data.length, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

@interface FixtureDelegate : NSObject <NSApplicationDelegate, NSTextFieldDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSTextField *input;
@property(nonatomic, strong) NSRunningApplication *previous;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) NSDate *started;
@property(nonatomic) BOOL stopping;
@end

@implementation FixtureDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    /* AppKit initialization can install its own termination handlers. Install
     * ours after launch so SIGTERM reaches the bounded main-loop cleanup. */
    signal(SIGTERM, request_stop);
    signal(SIGINT, request_stop);
    self.previous = NSWorkspace.sharedWorkspace.frontmostApplication;
    self.started = [NSDate date];
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(120, 180, 400, 210)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"DSCO owned desktop fixture";
    self.window.releasedWhenClosed = NO;
    self.window.minSize = NSMakeSize(260, 160);
    NSTextField *label = [NSTextField labelWithString:@"DSCO desktop control verification"];
    label.frame = NSMakeRect(20, 150, 350, 24);
    [self.window.contentView addSubview:label];
    self.input = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 95, 350, 32)];
    self.input.placeholderString = @"Owned fixture input";
    self.input.accessibilityIdentifier = @"dsco-fixture-input";
    self.input.delegate = self;
    self.input.autoresizingMask = NSViewWidthSizable;
    [self.window.contentView addSubview:self.input];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self.window makeFirstResponder:self.input];
    self.timer = [NSTimer scheduledTimerWithTimeInterval:0.05 target:self
        selector:@selector(tick:) userInfo:nil repeats:YES];
    emit(@{@"pid": @(getpid()), @"window_id": @(self.window.windowNumber)});
}

- (void)controlTextDidChange:(NSNotification *)notification {
    if (notification.object == self.input)
        emit(@{@"event": @"text", @"text": self.input.stringValue});
}

- (void)tick:(NSTimer *)timer {
    (void)timer;
    char command[32];
    ssize_t count = read(STDIN_FILENO, command, sizeof(command));
    if (count == 0 || (count > 0 && memchr(command, 'q', (size_t)count))) stop_requested = 1;
    if (!stop_requested && -self.started.timeIntervalSinceNow < 60) return;
    if (self.stopping) return;
    self.stopping = YES;
    [self.timer invalidate];
    BOOL wasFront = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == getpid();
    BOOL canRestore = self.previous && !self.previous.terminated && self.previous.processIdentifier != getpid();
    BOOL requested = wasFront && canRestore ? [self.previous activateWithOptions:0] : NO;
    BOOL restored = requested && NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == self.previous.processIdentifier;
    emit(@{@"event": @"closed", @"was_frontmost": @(wasFront),
           @"restore_available": @(canRestore), @"focus_restored": @(restored)});
    [self.window orderOut:nil];
    [NSApp terminate:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    /* AppKit may handle SIGTERM as an application termination event itself. */
    if (self.stopping) return;
    self.stopping = YES;
    [self.timer invalidate];
    BOOL wasFront = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == getpid();
    BOOL canRestore = self.previous && !self.previous.terminated && self.previous.processIdentifier != getpid();
    [self.window orderOut:nil];
    if (wasFront && canRestore) [self.previous activateWithOptions:0];
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:0.2];
    while (wasFront && canRestore && NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier != self.previous.processIdentifier && until.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    BOOL restored = canRestore && NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == self.previous.processIdentifier;
    emit(@{@"event": @"closed", @"was_frontmost": @(wasFront),
           @"restore_available": @(canRestore), @"focus_restored": @(restored)});
}
@end

int main(void) {
    @autoreleasepool {
        signal(SIGTERM, request_stop);
        signal(SIGINT, request_stop);
        signal(SIGPIPE, SIG_IGN);
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        FixtureDelegate *delegate = [FixtureDelegate new];
        NSApp.delegate = delegate;
        [NSApp run];
        /* A directly launched accessory process can return from run on a
         * termination signal without the delegate termination notification. */
        [delegate applicationWillTerminate:nil];
    }
    return 0;
}
