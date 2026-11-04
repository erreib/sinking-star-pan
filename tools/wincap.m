// List windows, or capture one by title/owner substring to PNG (works for windows on other Spaces).
//   wincap list | wincap cap <substring> <out.png>
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <dlfcn.h>
int main(int argc, char **argv) {
  @autoreleasepool {
    NSArray *list = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID));
    if (argc < 2 || !strcmp(argv[1], "list")) {
      for (NSDictionary *w in list) {
        int layer = [w[(id)kCGWindowLayer] intValue];
        BOOL on = [w[(id)kCGWindowIsOnscreen] boolValue];
        if (layer != 0 && !on) continue;
        NSDictionary *b = w[(id)kCGWindowBounds];
        printf("%d\t%s\t'%s'\tlayer=%d on=%d bounds=%d,%d %dx%d\n", [w[(id)kCGWindowNumber] intValue],
               [w[(id)kCGWindowOwnerName] UTF8String] ?: "?", [w[(id)kCGWindowName] UTF8String] ?: "", layer, on,
               [b[@"X"] intValue], [b[@"Y"] intValue], [b[@"Width"] intValue], [b[@"Height"] intValue]);
      }
      return 0;
    }
    NSString *needle = @(argv[2]);
    for (NSDictionary *w in list) {
      NSString *name = w[(id)kCGWindowName] ?: @""; NSString *owner = w[(id)kCGWindowOwnerName] ?: @"";
      if (![name containsString:needle] && ![owner containsString:needle]) continue;
      CGWindowID wid = [w[(id)kCGWindowNumber] unsignedIntValue];
      CGImageRef (*mk)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption) = dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
      CGImageRef img = mk ? mk(CGRectNull, kCGWindowListOptionIncludingWindow, wid, kCGWindowImageBoundsIgnoreFraming | kCGWindowImageBestResolution) : NULL;
      if (!img) { printf("capture failed for %u\n", wid); continue; }
      NSURL *url = [NSURL fileURLWithPath:@(argv[3])];
      CGImageDestinationRef dest = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL);
      CGImageDestinationAddImage(dest, img, NULL); CGImageDestinationFinalize(dest); CFRelease(dest);
      printf("captured %u '%s' (%s) %zux%zu -> %s\n", wid, name.UTF8String, owner.UTF8String, CGImageGetWidth(img), CGImageGetHeight(img), argv[3]);
      CGImageRelease(img);
      return 0;
    }
    printf("no window matching %s\n", argv[2]);
    return 1;
  }
}
