/* Background fixture for test_app_control_live.py. It never activates itself,
 * so every interaction must reach it without focus. Events go to stdout as
 * JSON lines; "q" on stdin or 60 s of runtime ends it. */
#import <Cocoa/Cocoa.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static void emit(NSDictionary *value) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];
    if (data) {
        fwrite(data.bytes, 1, data.length, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

@interface Fixture : NSObject <NSApplicationDelegate, NSTextFieldDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSTextField *input;
@property(nonatomic, strong) NSDate *started;
@end

@implementation Fixture
- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    self.started = [NSDate date];
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    NSMenu *bar = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenuItem *fixtureItem = [[NSMenuItem alloc] initWithTitle:@"Fixture" action:nil keyEquivalent:@""];
    NSMenu *fixtureMenu = [[NSMenu alloc] initWithTitle:@"Fixture"];
    NSMenuItem *reset = [[NSMenuItem alloc] initWithTitle:@"Reset Counter…" action:@selector(menuReset:) keyEquivalent:@""];
    reset.target = self;
    [fixtureMenu addItem:reset];
    fixtureItem.submenu = fixtureMenu;
    [bar addItem:fixtureItem];
    NSApp.mainMenu = bar;

    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(160, 160, 520, 340)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"DSCO app_control fixture";
    self.window.releasedWhenClosed = NO;
    NSView *root = self.window.contentView;

    NSTextField *label = [NSTextField labelWithString:@"Background control verification"];
    label.frame = NSMakeRect(20, 290, 400, 24);
    [root addSubview:label];

    self.input = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 240, 360, 30)];
    self.input.placeholderString = @"Recipient name";
    self.input.delegate = self;
    [root addSubview:self.input];

    NSButton *apply = [NSButton buttonWithTitle:@"Apply Changes" target:self action:@selector(apply:)];
    apply.frame = NSMakeRect(20, 190, 160, 32);
    [root addSubview:apply];

    NSButton *check = [NSButton checkboxWithTitle:@"Send receipt" target:self action:@selector(toggled:)];
    check.frame = NSMakeRect(200, 194, 200, 24);
    [root addSubview:check];

    /* Image-only button: no title and no accessibility label, so only a
     * visual description can find it. */
    NSImage *star = [NSImage imageWithSystemSymbolName:@"star.fill" accessibilityDescription:nil];
    NSImageSymbolConfiguration *big = [NSImageSymbolConfiguration configurationWithPointSize:48 weight:NSFontWeightBold];
    NSButton *icon = [NSButton buttonWithImage:[star imageWithSymbolConfiguration:big] target:self action:@selector(starred:)];
    icon.bordered = NO;
    icon.contentTintColor = NSColor.systemOrangeColor;
    icon.frame = NSMakeRect(400, 40, 90, 90);
    icon.accessibilityLabel = @"";
    [root addSubview:icon];

    [self.window orderFrontRegardless]; /* visible, never activated */
    [NSTimer scheduledTimerWithTimeInterval:0.05 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    emit(@{@"pid": @(getpid()), @"window_id": @(self.window.windowNumber)});
}

- (void)apply:(id)sender { (void)sender; emit(@{@"event": @"pressed", @"control": @"apply"}); }
- (void)starred:(id)sender { (void)sender; emit(@{@"event": @"pressed", @"control": @"star"}); }
- (void)menuReset:(id)sender { (void)sender; emit(@{@"event": @"menu", @"item": @"reset"}); }
- (void)toggled:(NSButton *)sender { emit(@{@"event": @"checkbox", @"state": @(sender.state == NSControlStateValueOn)}); }
- (void)controlTextDidChange:(NSNotification *)note {
    (void)note;
    emit(@{@"event": @"text", @"text": self.input.stringValue});
}

- (void)tick:(NSTimer *)timer {
    char buf[16];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    BOOL quit = n == 0 || (n > 0 && memchr(buf, 'q', (size_t)n)) || -self.started.timeIntervalSinceNow > 60;
    if (!quit) return;
    [timer invalidate];
    emit(@{@"event": @"closed", @"was_frontmost": @(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == getpid())});
    [NSApp terminate:nil];
}
@end

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        Fixture *delegate = [Fixture new];
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}
