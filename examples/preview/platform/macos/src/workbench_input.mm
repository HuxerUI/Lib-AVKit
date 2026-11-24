#include "workbench_media.h"

#include <cmath>

#import <AppKit/AppKit.h>
#include <huxerui/macos/platform_registry.h>

namespace avkit_preview {

std::function<void()> ObserveWorkbenchMagnification(
    huxerui::WindowHandle window, std::function<bool(huxerui::Point, double)> magnify) {
  __weak NSWindow* owner = huxerui::macos::GetPlatformWindow(window);
  if (!owner) return {};
  const auto monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskMagnify handler:^NSEvent*(NSEvent* event) {
    NSWindow* attached = owner;
    if (!attached || event.window != attached || attached.attachedSheet) return event;
    NSView* content = attached.contentView;
    const auto point = [content convertPoint:event.locationInWindow fromView:nil];
    const auto y = content.isFlipped ? point.y : NSHeight(content.bounds) - point.y;
    const auto scale = 1.0 + event.magnification;
    if (scale <= 0.0 || !std::isfinite(scale)) return event;
    return magnify({static_cast<float>(point.x), static_cast<float>(y)}, std::log2(scale)) ? nil : event;
  }];
  return [monitor] { if (monitor) [NSEvent removeMonitor:monitor]; };
}

} // namespace avkit_preview
