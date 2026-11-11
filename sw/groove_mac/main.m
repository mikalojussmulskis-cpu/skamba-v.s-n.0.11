// main.m - Groove Drums for macOS: a window around engine.c (the Plinky groove
// sequencer and drum engine). Build with ./build.sh, run build/GrooveDrums.app.
#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#include "engine.h"

enum { F_TUNE = 1, F_FENV = 13 }; // GF_TUNE .. GF_FENV (groove.h field numbers)
#define ROW_H 50
#define NAME_W 146
#define MUTE_X 150
#define STEP_X 196
#define STEP_W 34

static NSColor* rgb(int c) { return [NSColor colorWithSRGBRed:((c >> 16) & 255) / 255.0 green:((c >> 8) & 255) / 255.0 blue:(c & 255) / 255.0 alpha:1]; }
static void text(NSString* s, NSRect r, CGFloat size, NSColor* col, BOOL bold, NSTextAlignment al) {
	NSMutableParagraphStyle* p = [NSMutableParagraphStyle new];
	p.alignment = al;
	[s drawInRect:r withAttributes:@{ NSFontAttributeName: bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size],
		NSForegroundColorAttributeName: col, NSParagraphStyleAttributeName: p }];
}

// ---------------------------------------------------------------------------
@interface FlippedView : NSView
@end
@implementation FlippedView
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)r { [rgb(0x16181c) setFill]; NSRectFill(r); }
@end

// the Plinky screen (128 x 32), drawn at 3x: lane meters and the dancing cat
@interface OledView : NSView
@end
@implementation OledView
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)r {
	static uint8_t px[32 * 128];
	eng_oled(px);
	[NSColor.blackColor setFill];
	NSRectFill(self.bounds);
	[rgb(0x9fe8ff) setFill];
	for (int y = 0; y < 32; ++y)
		for (int x = 0; x < 128; ++x)
			if (px[y * 128 + x]) NSRectFill(NSMakeRect(x * 3, y * 3, 3, 3));
}
@end

static int step_x(int i) { return STEP_X + i * 36 + (i / 4) * 4; }

// ---------------------------------------------------------------------------
@interface GridView : NSView
@end

@implementation GridView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

- (void)hitAt:(NSPoint)p lane:(int*)lane step:(int*)step zone:(int*)zone {
	*lane = (int)(p.y / ROW_H);
	*step = -1;
	*zone = 0; // 1 name, 2 mute, 3 step
	if (*lane < 0 || *lane >= ENG_LANES || fmod(p.y, ROW_H) > ROW_H - 4) return;
	if (p.x < NAME_W) { *zone = 1; return; }
	if (p.x >= MUTE_X && p.x < MUTE_X + 40) { *zone = 2; return; }
	for (int i = 0; i < 16; ++i)
		if (p.x >= step_x(i) && p.x < step_x(i) + STEP_W) {
			*zone = 3;
			*step = eng_view_page() * 16 + i;
			return;
		}
}

- (void)drawRect:(NSRect)dirty {
	[rgb(0x16181c) setFill];
	NSRectFill(self.bounds);
	int sel = eng_lane_sel(), page = eng_view_page();
	int lk = eng_lock_step_idx(), lkl = eng_lock_lane();
	BOOL blink = ((long)([NSDate timeIntervalSinceReferenceDate] * 4)) & 1;
	for (int l = 0; l < ENG_LANES; ++l) {
		CGFloat y = l * ROW_H;
		// name + activity
		NSRect nr = NSMakeRect(0, y, NAME_W, ROW_H - 4);
		[(l == sel ? rgb(0x34405a) : rgb(0x23272e)) setFill];
		[[NSBezierPath bezierPathWithRoundedRect:nr xRadius:5 yRadius:5] fill];
		int fl = eng_flash(l);
		[[rgb(0xffa040) colorWithAlphaComponent:fl / 255.0] setFill];
		NSRectFill(NSMakeRect(6, y + ROW_H - 10, (NAME_W - 12) * fl / 255.0, 3));
		text([NSString stringWithFormat:@"%d  %s", l + 1, eng_lane_model_name(l)], NSMakeRect(10, y + 12, NAME_W - 14, 20), 14,
			l == sel ? NSColor.whiteColor : rgb(0xc8ccd4), YES, NSTextAlignmentLeft);
		// mute
		NSRect mr = NSMakeRect(MUTE_X, y, 40, ROW_H - 4);
		[(eng_muted(l) ? rgb(0x8a2a2a) : rgb(0x23272e)) setFill];
		[[NSBezierPath bezierPathWithRoundedRect:mr xRadius:5 yRadius:5] fill];
		text(@"M", NSMakeRect(MUTE_X, y + 13, 40, 20), 13, eng_muted(l) ? NSColor.whiteColor : rgb(0x6a707a), YES, NSTextAlignmentCenter);
		// steps
		int start = eng_loop_start(l), len = eng_loop_len(l), pos = eng_lane_pos(l), acid = eng_is_acid(l);
		for (int i = 0; i < 16; ++i) {
			int idx = page * 16 + i, s = eng_step(l, idx);
			int inloop = ((idx - start) & 63) < len;
			NSRect r = NSMakeRect(step_x(i), y, STEP_W, ROW_H - 4);
			NSColor* c = inloop ? ((i & 4) ? rgb(0x2a2e36) : rgb(0x30353e)) : rgb(0x1b1d21);
			if (s & 0x80) {
				int v = (s >> 4) & 7; if (!v) v = 5;
				c = v == 7 ? rgb(0xff7a30) : [rgb(0xe8b04a) blendedColorWithFraction:(7 - v) * 0.11 ofColor:rgb(0x30353e)];
				if (!inloop) c = [c blendedColorWithFraction:0.7 ofColor:rgb(0x1b1d21)];
			}
			[c setFill];
			[[NSBezierPath bezierPathWithRoundedRect:r xRadius:4 yRadius:4] fill];
			if ((s & 0x80) && acid) { // note degree and slide
				text([NSString stringWithFormat:@"%d", s & 7], NSMakeRect(r.origin.x, y + 6, STEP_W, 16), 12, rgb(0x1a1a1a), YES, NSTextAlignmentCenter);
				if (s & 8) { [rgb(0x1a1a1a) setFill]; NSRectFill(NSMakeRect(r.origin.x + 6, y + ROW_H - 14, STEP_W - 4, 3)); }
			}
			if ((s & 0x80) && eng_step_locked(l, idx)) { [NSColor.whiteColor setFill]; [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(r.origin.x + STEP_W - 9, y + 4, 5, 5)] fill]; }
			if (idx == pos) { [NSColor.whiteColor setStroke]; NSBezierPath* b = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 1, 1) xRadius:4 yRadius:4]; b.lineWidth = 2; [b stroke]; }
			if (lk == idx && lkl == l && blink) { [rgb(0x40e0ff) setStroke]; NSBezierPath* b = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 1.5, 1.5) xRadius:4 yRadius:4]; b.lineWidth = 3; [b stroke]; }
		}
	}
}

- (void)mouseDown:(NSEvent*)e {
	[self.window makeFirstResponder:self];
	int l, s, z;
	[self hitAt:[self convertPoint:e.locationInWindow fromView:nil] lane:&l step:&s zone:&z];
	NSEventModifierFlags m = e.modifierFlags;
	if (z == 1) { eng_select_lane(l); eng_trigger(l, 7); }
	else if (z == 2) eng_toggle_mute(l);
	else if (z == 3) {
		if (m & NSEventModifierFlagControl) eng_lock_step(l, s);
		else if (m & NSEventModifierFlagOption) eng_toggle_accent(l, s);
		else if (m & NSEventModifierFlagShift) eng_toggle_slide(l, s);
		else { eng_select_lane(l); eng_toggle_step(l, s); }
	}
	[self setNeedsDisplay:YES];
}
- (void)rightMouseDown:(NSEvent*)e {
	int l, s, z;
	[self hitAt:[self convertPoint:e.locationInWindow fromView:nil] lane:&l step:&s zone:&z];
	if (z == 3) eng_lock_step(l, s);
	[self setNeedsDisplay:YES];
}
- (void)scrollWheel:(NSEvent*)e {
	static CGFloat acc;
	int l, s, z;
	[self hitAt:[self convertPoint:e.locationInWindow fromView:nil] lane:&l step:&s zone:&z];
	if (z != 3) return;
	acc += e.scrollingDeltaY * (e.hasPreciseScrollingDeltas ? 0.1 : 1.0);
	while (acc >= 1) { eng_step_wheel(l, s, 1); acc -= 1; }
	while (acc <= -1) { eng_step_wheel(l, s, -1); acc += 1; }
	[self setNeedsDisplay:YES];
}
@end

// ---------------------------------------------------------------------------
@interface App : NSObject <NSApplicationDelegate>
@end

@implementation App {
	NSWindow* win;
	GridView* grid;
	OledView* oled;
	NSButton* playBtn;
	NSSlider* bpmSlider; NSTextField* bpmLabel;
	NSPopUpButton* slotPop;
	NSSlider* bus[4]; NSTextField* busVal[4];
	NSTextField* title; NSTextField* status; NSTextField* pageLabel; NSTextField* loopLabel;
	NSPopUpButton* modelPop; NSPopUpButton* condPop; NSButton* clearLocksBtn; NSButton* newLineBtn;
	NSSlider* fs[14]; NSTextField* fv[14];
	AudioQueueRef queue;
	int shownSel, shownLock, shownModel;
}

static void audio_cb(void* ud, AudioQueueRef q, AudioQueueBufferRef b) {
	int frames = b->mAudioDataBytesCapacity / 8;
	eng_render((float*)b->mAudioData, frames);
	b->mAudioDataByteSize = frames * 8;
	AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

- (NSTextField*)label:(NSString*)s frame:(NSRect)r size:(CGFloat)sz {
	NSTextField* t = [NSTextField labelWithString:s];
	t.frame = r;
	t.font = [NSFont systemFontOfSize:sz];
	t.textColor = rgb(0xc8ccd4);
	[win.contentView addSubview:t];
	return t;
}
- (NSButton*)button:(NSString*)s frame:(NSRect)r action:(SEL)a {
	NSButton* b = [NSButton buttonWithTitle:s target:self action:a];
	b.frame = r;
	[win.contentView addSubview:b];
	return b;
}
- (NSSlider*)slider:(NSRect)r min:(double)lo max:(double)hi tag:(NSInteger)tag action:(SEL)a {
	NSSlider* s = [NSSlider sliderWithValue:lo minValue:lo maxValue:hi target:self action:a];
	s.frame = r; s.tag = tag; s.continuous = YES;
	[win.contentView addSubview:s];
	return s;
}

- (void)applicationDidFinishLaunching:(NSNotification*)n {
	NSString* dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/GrooveDrums"];
	[NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
	eng_init([[dir stringByAppendingPathComponent:@"patterns.bin"] fileSystemRepresentation]);

	win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1250, 790)
		styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
	win.title = @"Groove Drums (Plinky engine)";
	win.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
	win.backgroundColor = rgb(0x16181c);
	NSView* cv = [[FlippedView alloc] initWithFrame:win.contentView.bounds];
	win.contentView = cv;

	// top bar
	playBtn = [self button:@"▶ Play" frame:NSMakeRect(10, 12, 90, 30) action:@selector(play:)];
	[self label:@"BPM" frame:NSMakeRect(112, 18, 34, 18) size:12];
	bpmSlider = [self slider:NSMakeRect(146, 16, 130, 22) min:60 max:200 tag:0 action:@selector(bpm:)];
	bpmSlider.doubleValue = 120;
	bpmLabel = [self label:@"120" frame:NSMakeRect(280, 18, 40, 18) size:12];
	slotPop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(326, 13, 120, 28) pullsDown:NO];
	for (int i = 0; i < 24; ++i) [slotPop addItemWithTitle:[NSString stringWithFormat:@"Pattern %d", i + 1]];
	slotPop.target = self; slotPop.action = @selector(slot:);
	[cv addSubview:slotPop];
	NSString* bn[4] = { @"Drum Vol", @"Comp", @"DJ Filter", @"Drive" };
	for (int i = 0; i < 4; ++i) {
		CGFloat x = 456 + i * 198;
		[self label:bn[i] frame:NSMakeRect(x, 18, 60, 18) size:12];
		bus[i] = [self slider:NSMakeRect(x + 60, 16, 92, 22) min:0 max:255 tag:i action:@selector(busChanged:)];
		busVal[i] = [self label:@"" frame:NSMakeRect(x + 156, 18, 42, 18) size:12];
	}

	// grid
	grid = [[GridView alloc] initWithFrame:NSMakeRect(10, 60, 790, ENG_LANES * ROW_H)];
	[cv addSubview:grid];
	CGFloat by = 60 + ENG_LANES * ROW_H + 8;
	[self button:@"◀" frame:NSMakeRect(196, by, 40, 28) action:@selector(pagePrev:)];
	pageLabel = [self label:@"" frame:NSMakeRect(240, by + 6, 120, 18) size:12];
	[self button:@"▶" frame:NSMakeRect(360, by, 40, 28) action:@selector(pageNext:)];
	loopLabel = [self label:@"" frame:NSMakeRect(420, by + 6, 170, 18) size:12];
	[self button:@"start −" frame:NSMakeRect(420, by + 28, 70, 26) action:@selector(loopStart:)].tag = -1;
	[self button:@"start +" frame:NSMakeRect(490, by + 28, 70, 26) action:@selector(loopStart:)].tag = 1;
	[self button:@"end −" frame:NSMakeRect(570, by + 28, 66, 26) action:@selector(loopEnd:)].tag = -1;
	[self button:@"end +" frame:NSMakeRect(636, by + 28, 66, 26) action:@selector(loopEnd:)].tag = 1;
	[self button:@"Clear lane" frame:NSMakeRect(10, by, 110, 28) action:@selector(clearLane:)];
	status = [self label:@"" frame:NSMakeRect(10, by + 34, 400, 20) size:13];
	[self label:@"Plinky screen (meters, cat and boombox; text not shown)" frame:NSMakeRect(10, 560, 384, 18) size:11];
	oled = [[OledView alloc] initWithFrame:NSMakeRect(10, 560 + 20, 384, 96)];
	[cv addSubview:oled];
	status.textColor = rgb(0xffb060);

	// sound panel
	CGFloat px = 830;
	title = [self label:@"" frame:NSMakeRect(px, 58, 400, 24) size:17];
	title.font = [NSFont boldSystemFontOfSize:17];
	title.textColor = NSColor.whiteColor;
	[self label:@"Drum Type" frame:NSMakeRect(px, 94, 90, 18) size:12];
	modelPop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(px + 96, 88, 160, 28) pullsDown:NO];
	for (int i = 0; i < eng_model_count(); ++i) [modelPop addItemWithTitle:[NSString stringWithUTF8String:eng_model_name_at(i)]];
	modelPop.target = self; modelPop.action = @selector(model:);
	[cv addSubview:modelPop];
	newLineBtn = [self button:@"New acid line" frame:NSMakeRect(px + 264, 88, 120, 28) action:@selector(newLine:)];
	[self label:@"Condition" frame:NSMakeRect(px, 126, 90, 18) size:12];
	condPop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(px + 96, 120, 160, 28) pullsDown:NO];
	for (int i = 0; i < eng_cond_count(); ++i) [condPop addItemWithTitle:[NSString stringWithUTF8String:eng_cond_name(i)]];
	condPop.target = self; condPop.action = @selector(cond:);
	[cv addSubview:condPop];
	clearLocksBtn = [self button:@"Clear locks" frame:NSMakeRect(px + 264, 120, 110, 28) action:@selector(clearLocks:)];
	for (int f = F_TUNE; f <= F_FENV; ++f) {
		CGFloat y = 160 + (f - F_TUNE) * 30;
		[self label:[NSString stringWithUTF8String:eng_field_name(f)] frame:NSMakeRect(px, y + 2, 92, 18) size:12];
		fs[f] = [self slider:NSMakeRect(px + 96, y, 220, 22) min:(f == F_TUNE ? -120 : 0) max:(f == F_TUNE ? 120 : 255) tag:f action:@selector(field:)];
		fv[f] = [self label:@"" frame:NSMakeRect(px + 324, y + 2, 70, 18) size:12];
	}

	NSTextField* help = [self label:
		@"Click a step: on / off.   Right-click (or ⌃-click) a step: step lock — the sliders and Condition then edit only that step (click it again or Esc to leave).\n"
		@"Scroll on a step: velocity (Acid lanes: note).   ⌥-click: accent.   ⇧-click: slide (Acid).   Click a lane name: select + play it.   M: mute.\n"
		@"Keys 1–8: play lanes live.   Space: play / stop.   ← →: step page.   Delete: clear lane (first the steps, then the sound).   Choosing Acid makes a new line.\n"
		@"Filter, Filter Env and DJ Filter are off in the middle.   Delay and reverb here are simple stand-ins for Plinky's own effects."
		frame:NSMakeRect(10, 694, 1230, 84) size:11.5];
	help.textColor = rgb(0x8a909a);

	[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* e) { return [self key:e] ? nil : e; }];
	[NSTimer scheduledTimerWithTimeInterval:1.0 / 30 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
	shownSel = shownLock = shownModel = -99;
	[self refreshPanel:YES];
	[win center];
	[win makeKeyAndOrderFront:nil];
	[win makeFirstResponder:grid];
	[NSApp activateIgnoringOtherApps:YES];

	AudioStreamBasicDescription fmt = { 0 };
	fmt.mSampleRate = ENG_SR;
	fmt.mFormatID = kAudioFormatLinearPCM;
	fmt.mFormatFlags = kLinearPCMFormatFlagIsFloat | kLinearPCMFormatFlagIsPacked;
	fmt.mBytesPerPacket = 8; fmt.mFramesPerPacket = 1; fmt.mBytesPerFrame = 8;
	fmt.mChannelsPerFrame = 2; fmt.mBitsPerChannel = 32;
	if (AudioQueueNewOutput(&fmt, audio_cb, NULL, NULL, NULL, 0, &queue) == noErr) {
		for (int i = 0; i < 3; ++i) {
			AudioQueueBufferRef b;
			AudioQueueAllocateBuffer(queue, 512 * 8, &b);
			audio_cb(NULL, queue, b);
		}
		AudioQueueStart(queue, NULL);
	}
	else
		status.stringValue = @"could not open audio output";

	// developer check: GrooveDrums --snapshot file.png [play] draws the window into a PNG and quits
	NSArray* args = NSProcessInfo.processInfo.arguments;
	NSUInteger si = [args indexOfObject:@"--snapshot"];
	if (si != NSNotFound && si + 1 < args.count) {
		NSString* path = args[si + 1];
		if ([args containsObject:@"play"]) {
			for (int i = 0; i < 16; i += 4) eng_toggle_step(0, i);
			eng_toggle_step(1, 4); eng_toggle_step(1, 12);
			for (int i = 0; i < 16; i += 2) eng_toggle_step(3, i);
			eng_select_lane(7); eng_acid_new_line();
			eng_play_toggle();
		}
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
			NSView* v = self->win.contentView;
			NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
			[v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
			[[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
			[NSApp terminate:nil];
		});
	}
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a { return YES; }
- (void)applicationWillTerminate:(NSNotification*)n {
	if (queue) AudioQueueStop(queue, true);
	eng_save_now();
}

- (BOOL)key:(NSEvent*)e {
	NSString* c = e.charactersIgnoringModifiers;
	if (!c.length || (e.modifierFlags & NSEventModifierFlagCommand)) return NO;
	unichar k = [c characterAtIndex:0];
	if (k == ' ') { eng_play_toggle(); return YES; }
	if (k >= '1' && k <= '8') { if (!e.isARepeat) eng_trigger(k - '1', 7); return YES; }
	if (k == 27) { eng_lock_exit(); return YES; }
	if (k == NSLeftArrowFunctionKey) { eng_page(-1); return YES; }
	if (k == NSRightArrowFunctionKey) { eng_page(1); return YES; }
	if (k == 127 || k == NSDeleteFunctionKey) { eng_clear_lane(); [self refreshPanel:YES]; return YES; }
	return NO;
}

- (void)play:(id)s { eng_play_toggle(); }
- (void)bpm:(NSSlider*)s { eng_set_bpm(s.doubleValue); bpmLabel.stringValue = [NSString stringWithFormat:@"%.0f", s.doubleValue]; }
- (void)slot:(NSPopUpButton*)p { eng_select_slot((int)p.indexOfSelectedItem); }
- (void)busChanged:(NSSlider*)s { eng_bus_set((int)s.tag, (int)lround(s.doubleValue)); }
- (void)pagePrev:(id)s { eng_page(-1); }
- (void)pageNext:(id)s { eng_page(1); }
- (void)loopStart:(NSButton*)b { int l = eng_lane_sel(); eng_set_loop_start(l, eng_loop_start(l) + (int)b.tag); }
- (void)loopEnd:(NSButton*)b { int l = eng_lane_sel(); eng_set_loop_end(l, eng_loop_start(l) + eng_loop_len(l) - 1 + (int)b.tag); }
- (void)clearLane:(id)s { eng_clear_lane(); [self refreshPanel:YES]; }
- (void)model:(NSPopUpButton*)p { eng_set_model(eng_lane_sel(), (int)p.indexOfSelectedItem); }
- (void)newLine:(id)s { eng_acid_new_line(); }
- (void)cond:(NSPopUpButton*)p { eng_cond_set((int)p.indexOfSelectedItem); }
- (void)clearLocks:(id)s { eng_lock_clear(); [self refreshPanel:YES]; }
- (void)field:(NSSlider*)s {
	eng_field_set((int)s.tag, (int)lround(s.doubleValue));
	char b[32];
	fv[s.tag].stringValue = [NSString stringWithUTF8String:eng_field_str((int)s.tag, b)];
}

- (void)refreshPanel:(BOOL)values {
	int sel = eng_lane_sel(), lk = eng_lock_step_idx();
	title.stringValue = lk >= 0 ? [NSString stringWithFormat:@"Lock step %d  ·  lane %d %s", lk + 1, eng_lock_lane() + 1, eng_lane_model_name(eng_lock_lane())]
	                            : [NSString stringWithFormat:@"Lane %d  ·  %s", sel + 1, eng_lane_model_name(sel)];
	title.textColor = lk >= 0 ? rgb(0x40e0ff) : NSColor.whiteColor;
	modelPop.enabled = lk < 0;
	condPop.enabled = lk >= 0;
	clearLocksBtn.enabled = lk >= 0;
	newLineBtn.hidden = !eng_is_acid(sel) || lk >= 0;
	[modelPop selectItemAtIndex:eng_model_pos(sel)];
	[condPop selectItemAtIndex:lk >= 0 ? eng_cond_get() : 0];
	char b[32];
	for (int f = F_TUNE; f <= F_FENV; ++f) {
		if (values) fs[f].doubleValue = eng_field_get(f);
		fv[f].stringValue = [NSString stringWithUTF8String:eng_field_str(f, b)];
	}
	for (int i = 0; i < 4; ++i) {
		if (values) bus[i].doubleValue = eng_bus_get(i);
		busVal[i].stringValue = [NSString stringWithUTF8String:eng_bus_str(i, b)];
	}
}

- (void)tick:(NSTimer*)t {
	eng_pump();
	[grid setNeedsDisplay:YES];
	[oled setNeedsDisplay:YES];
	int st = eng_playing();
	playBtn.title = st == 1 ? @"■ Stop" : st == 2 ? @"… bar" : @"▶ Play";
	int pend = eng_pending_slot();
	if (slotPop.indexOfSelectedItem != (pend >= 0 ? pend : eng_slot()) && !NSEvent.pressedMouseButtons)
		[slotPop selectItemAtIndex:pend >= 0 ? pend : eng_slot()];
	int sel = eng_lane_sel(), lk = eng_lock_step_idx(), model = eng_model_pos(sel);
	BOOL changed = sel != shownSel || lk != shownLock || model != shownModel;
	shownSel = sel; shownLock = lk; shownModel = model;
	[self refreshPanel:changed || !NSEvent.pressedMouseButtons];
	int p = eng_view_page(), np = 0;
	for (int i = 0; i < 4; ++i) np += eng_page_in_loop(i);
	pageLabel.stringValue = [NSString stringWithFormat:@"steps %d–%d%@", p * 16 + 1, p * 16 + 16, np > 1 ? @"" : @"  (1 page)"];
	int ls = eng_loop_start(sel), ll = eng_loop_len(sel);
	loopLabel.stringValue = [NSString stringWithFormat:@"lane %d loop: %d–%d (%d steps)", sel + 1, ls + 1, ((ls + ll - 1) & 63) + 1, ll];
	char m[48], sub[48];
	int age = eng_message(m, sizeof m, sub, sizeof sub);
	status.stringValue = age < 2000 ? [NSString stringWithFormat:@"%s  %s", m, sub] : @"";
}
@end


int main(int argc, const char** argv) {
	@autoreleasepool {
		NSApplication* app = NSApplication.sharedApplication;
		app.activationPolicy = NSApplicationActivationPolicyRegular;
		App* d = [App new];
		app.delegate = d;
		NSMenu* bar = [NSMenu new];
		NSMenuItem* item = [NSMenuItem new];
		[bar addItem:item];
		NSMenu* m = [NSMenu new];
		[m addItemWithTitle:@"Quit Groove Drums" action:@selector(terminate:) keyEquivalent:@"q"];
		item.submenu = m;
		app.mainMenu = bar;
		[app run];
	}
	return 0;
}
