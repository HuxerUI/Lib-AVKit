#pragma once

/// @file
/// @brief HuxerUI application integration for Lib-AVKit.
///
/// Link HuxerUI::AVKit to use this application hook. Headless consumers can link AVKit::Core.

#include <huxerui/app.h>

namespace huxerui::avkit {

/// @brief Installs the library's application integration from a HuxerUI application hook.
/// @param context Context supplied by HuxerUI during application setup.
void Install(ApplicationContext& context);

} // namespace huxerui::avkit
