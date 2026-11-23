#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wwe2k14::launcher::internal {

struct PaintColor {
  uint8_t r = 0, g = 0, b = 0, a = 255;
  bool operator==(const PaintColor&) const = default;
};

struct PaintPoint {
  int x = 0, y = 0;
};

enum class PaintClickDrawTool : uint8_t { None, Brush, Shape };
enum class PaintClickDrawAction : uint8_t {
  None,
  StartBrush,
  ExtendBrush,
  StopBrush,
  PreviewShape,
  FinishShape,
  CancelShape,
};

struct PaintClickDrawInput {
  bool double_click = false;
  bool click = false;
  bool escape = false;
  bool inside = false;
  bool reuse_brush_checkpoint = false;
  PaintPoint brush_checkpoint{};
  PaintPoint point{};
};

struct PaintClickDrawUpdate {
  PaintClickDrawAction action = PaintClickDrawAction::None;
  PaintPoint from{};
  PaintPoint to{};
  bool record_history = false;
  bool stop_after = false;
};

// The no-hold canvas gesture: a double-click starts a brush stroke or places a
// shape start point; a later click ends it. This state is independent of ImGui.
class PaintClickDrawState {
 public:
  PaintClickDrawUpdate Update(PaintClickDrawTool tool, const PaintClickDrawInput& input);
  bool Active() const { return kind_ != Kind::None; }
  bool BrushActive() const { return kind_ == Kind::Brush; }
  bool ShapeActive() const { return kind_ == Kind::Shape; }
  void Cancel() { kind_ = Kind::None; }

 private:
  enum class Kind : uint8_t { None, Brush, Shape };
  Kind kind_ = Kind::None;
  PaintPoint last_point_{};
  bool brush_reanchor_ = false;
  PaintPoint shape_start_{};
  PaintPoint shape_preview_{};
};

// Straight-alpha RGBA8 canvas. The editor uses the real 128x128 or 256x256 slot dimensions.
struct PaintCanvas {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;
  bool Valid() const;
};

PaintCanvas MakePaintCanvas(int width, int height, PaintColor clear = {0, 0, 0, 0});
bool ClearPaintCanvas(PaintCanvas* canvas, PaintColor color = {0, 0, 0, 0});
bool PaintBrush(PaintCanvas* canvas, float x, float y, float size, float hardness, PaintColor color,
                bool erase = false);
bool PaintLine(PaintCanvas* canvas, PaintPoint from, PaintPoint to, float size, float hardness,
               PaintColor color, bool erase = false);
bool PaintRectangle(PaintCanvas* canvas, PaintPoint from, PaintPoint to, bool filled, float size,
                    float hardness, PaintColor color);
bool PaintEllipse(PaintCanvas* canvas, PaintPoint from, PaintPoint to, bool filled, float size,
                  float hardness, PaintColor color);
bool PaintFill(PaintCanvas* canvas, int x, int y, PaintColor color);
PaintColor PickPaintColor(const PaintCanvas& canvas, int x, int y);
bool PaintMask(PaintCanvas* canvas, int x, int y, int mask_width, int mask_height,
               const std::vector<uint8_t>& alpha_mask, PaintColor color);
bool CompositePaintImage(PaintCanvas* destination, const PaintCanvas& source, int x, int y,
                         int width, int height);

// Full-canvas snapshots keep the history implementation independent of UI and renderer state.
class PaintHistory {
 public:
  explicit PaintHistory(size_t capacity = 30);
  void Record(const PaintCanvas& canvas);
  bool Undo(PaintCanvas* canvas);
  bool Redo(PaintCanvas* canvas);
  void Clear();
  size_t UndoCount() const { return undo_.size(); }
  size_t RedoCount() const { return redo_.size(); }

 private:
  size_t capacity_;
  std::vector<std::vector<uint8_t>> undo_;
  std::vector<std::vector<uint8_t>> redo_;
  void Push(std::vector<std::vector<uint8_t>>* stack, const std::vector<uint8_t>& pixels);
};

}  // namespace wwe2k14::launcher::internal
