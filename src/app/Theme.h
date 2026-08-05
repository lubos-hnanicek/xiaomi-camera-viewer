#pragma once

#include <imgui.h>

namespace xv::theme {

// A dark theme tuned for sitting behind video: low-chroma surfaces so the tiles
// stay the brightest thing on screen, and a single accent used sparingly.
void apply(float scale);

// Semantic colours the views share.
inline constexpr ImVec4 kAccent{0.31f, 0.60f, 0.94f, 1.00f};
inline constexpr ImVec4 kLive{0.30f, 0.78f, 0.45f, 1.00f};
inline constexpr ImVec4 kPending{0.94f, 0.72f, 0.30f, 1.00f};
inline constexpr ImVec4 kFailed{0.90f, 0.36f, 0.36f, 1.00f};
inline constexpr ImVec4 kMuted{0.55f, 0.57f, 0.62f, 1.00f};
inline constexpr ImVec4 kTileBackground{0.07f, 0.07f, 0.09f, 1.00f};

} // namespace xv::theme
