#pragma once

namespace UI {
  void build();
  void setActive(bool on);      // false while the screen is dark: pauses animations
  void refreshLibrary();        // after a rescan
}
