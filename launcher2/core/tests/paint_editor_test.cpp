#ifdef NDEBUG
#undef NDEBUG
#endif

#include "internal/paint_editor.h"

#include <cassert>
#include <iostream>

using namespace wwe2k14::launcher::internal;

namespace {

PaintColor Pixel(const PaintCanvas& canvas, int x, int y) { return PickPaintColor(canvas, x, y); }

}  // namespace

int main() {
  const PaintColor transparent{0, 0, 0, 0};
  const PaintColor red{255, 0, 0, 255};
  const PaintColor green{0, 255, 0, 255};
  PaintCanvas canvas = MakePaintCanvas(8, 8, transparent);
  assert(canvas.Valid());

  // Single-pixel brush and straight-alpha source-over produce exact RGBA values.
  assert(PaintBrush(&canvas, 2, 2, 1.0f, 1.0f, red));
  assert(Pixel(canvas, 2, 2) == red);
  assert(PaintBrush(&canvas, 2, 2, 1.0f, 1.0f, {0, 0, 255, 128}));
  assert(Pixel(canvas, 2, 2) == (PaintColor{127, 0, 128, 255}));
  assert(PaintBrush(&canvas, 2, 2, 1.0f, 1.0f, red, true));
  assert(Pixel(canvas, 2, 2) == transparent);

  // Bresenham line includes both endpoints; filled and outlined rectangles have predictable bounds.
  assert(PaintLine(&canvas, {1, 1}, {5, 1}, 1.0f, 1.0f, red));
  for (int x = 1; x <= 5; ++x) assert(Pixel(canvas, x, 1) == red);
  ClearPaintCanvas(&canvas, transparent);
  assert(PaintRectangle(&canvas, {1, 1}, {4, 4}, true, 1, 1, green));
  assert(Pixel(canvas, 1, 1) == green && Pixel(canvas, 4, 4) == green);
  assert(Pixel(canvas, 0, 1) == transparent && Pixel(canvas, 5, 4) == transparent);
  ClearPaintCanvas(&canvas, transparent);
  assert(PaintRectangle(&canvas, {1, 1}, {5, 5}, false, 1, 1, red));
  assert(Pixel(canvas, 3, 1) == red && Pixel(canvas, 5, 3) == red);
  assert(Pixel(canvas, 3, 3) == transparent);

  // Ellipse fill stays inside its bounding box; the outline reaches the cardinal points.
  ClearPaintCanvas(&canvas, transparent);
  assert(PaintEllipse(&canvas, {1, 1}, {5, 5}, true, 1, 1, green));
  assert(Pixel(canvas, 3, 3) == green && Pixel(canvas, 1, 1) == transparent);
  ClearPaintCanvas(&canvas, transparent);
  assert(PaintEllipse(&canvas, {1, 1}, {5, 5}, false, 1, 1, red));
  assert(Pixel(canvas, 3, 1) == red && Pixel(canvas, 5, 3) == red);
  assert(Pixel(canvas, 3, 3) == transparent);

  // A closed outline confines an exact four-neighbour flood fill.
  ClearPaintCanvas(&canvas, transparent);
  assert(PaintRectangle(&canvas, {1, 1}, {6, 6}, false, 1, 1, red));
  assert(PaintFill(&canvas, 3, 3, green));
  assert(Pixel(canvas, 3, 3) == green && Pixel(canvas, 0, 0) == transparent);
  assert(Pixel(canvas, 1, 3) == red);
  assert(!PaintFill(&canvas, -1, 0, red));
  assert(PickPaintColor(canvas, 3, 3) == green);

  // Text is rasterized by the UI font atlas into an alpha mask, then composited by the core.
  ClearPaintCanvas(&canvas, transparent);
  assert(PaintMask(&canvas, 2, 3, 2, 2, {255, 128, 0, 255}, red));
  assert(Pixel(canvas, 2, 3) == red);
  assert(Pixel(canvas, 3, 3) == (PaintColor{255, 0, 0, 128}));
  assert(Pixel(canvas, 2, 4) == transparent);

  // Imported pixels can be moved and scaled with nearest-neighbour sampling and alpha compositing.
  PaintCanvas source = MakePaintCanvas(2, 1, transparent);
  PaintBrush(&source, 0, 0, 1, 1, red);
  PaintBrush(&source, 1, 0, 1, 1, {0, 0, 255, 255});
  ClearPaintCanvas(&canvas, transparent);
  assert(CompositePaintImage(&canvas, source, 1, 2, 4, 2));
  assert(Pixel(canvas, 1, 2) == red && Pixel(canvas, 2, 2) == red);
  assert(Pixel(canvas, 3, 2) == (PaintColor{0, 0, 255, 255}));
  assert(Pixel(canvas, 4, 3) == (PaintColor{0, 0, 255, 255}));
  assert(Pixel(canvas, 0, 2) == transparent);

  // History restores full pixel snapshots, supports redo, and retains at least the latest 30 steps.
  PaintCanvas history_canvas = MakePaintCanvas(4, 4, transparent);
  PaintHistory history;
  history.Record(history_canvas);
  PaintBrush(&history_canvas, 0, 0, 1, 1, red);
  history.Record(history_canvas);
  PaintBrush(&history_canvas, 1, 0, 1, 1, green);
  assert(history.Undo(&history_canvas));
  assert(Pixel(history_canvas, 0, 0) == red && Pixel(history_canvas, 1, 0) == transparent);
  assert(history.Redo(&history_canvas));
  assert(Pixel(history_canvas, 1, 0) == green);
  for (int i = 0; i < 35; ++i) {
    history.Record(history_canvas);
    ClearPaintCanvas(&history_canvas, {static_cast<uint8_t>(i), 0, 0, 255});
  }
  assert(history.UndoCount() == 30);
  assert(!history_canvas.rgba.empty());

  // A double-click can reuse the first tap's undo checkpoint, then follow the pointer
  // without a held button. One click stops the stroke and one undo restores the canvas.
  PaintCanvas click_draw_canvas = MakePaintCanvas(12, 12, transparent);
  PaintHistory click_draw_history;
  click_draw_history.Record(click_draw_canvas);
  assert(PaintBrush(&click_draw_canvas, 2, 2, 1.0f, 1.0f, red));
  PaintClickDrawState click_draw;
  PaintClickDrawInput gesture{};
  gesture.double_click = true;
  gesture.click = true;
  gesture.inside = true;
  gesture.point = {2, 2};
  gesture.reuse_brush_checkpoint = true;
  gesture.brush_checkpoint = {2, 2};
  auto gesture_update = click_draw.Update(PaintClickDrawTool::Brush, gesture);
  assert(gesture_update.action == PaintClickDrawAction::StartBrush && !gesture_update.record_history);
  assert(click_draw.BrushActive());
  gesture = {};
  gesture.inside = true;
  gesture.point = {8, 2};
  gesture_update = click_draw.Update(PaintClickDrawTool::Brush, gesture);
  assert(gesture_update.action == PaintClickDrawAction::ExtendBrush);
  assert(gesture_update.from.x == 2 && gesture_update.to.x == 8);
  assert(PaintLine(&click_draw_canvas, gesture_update.from, gesture_update.to, 1.0f, 1.0f, red));
  gesture.click = true;
  gesture_update = click_draw.Update(PaintClickDrawTool::Brush, gesture);
  assert(gesture_update.action == PaintClickDrawAction::StopBrush && !click_draw.Active());
  assert(click_draw_history.UndoCount() == 1);
  assert(click_draw_history.Undo(&click_draw_canvas));
  assert(Pixel(click_draw_canvas, 2, 2) == transparent && Pixel(click_draw_canvas, 8, 2) == transparent);

  gesture = {};
  gesture.double_click = true;
  gesture.click = true;
  gesture.inside = true;
  gesture.point = {3, 3};
  gesture_update = click_draw.Update(PaintClickDrawTool::Brush, gesture);
  assert(gesture_update.action == PaintClickDrawAction::StartBrush && gesture_update.record_history);
  gesture = {};
  gesture.point = {15, 3};
  gesture_update = click_draw.Update(PaintClickDrawTool::Brush, gesture);
  assert(gesture_update.action == PaintClickDrawAction::None);
  gesture.inside = true;
  gesture.point = {4, 3};
  gesture_update = click_draw.Update(PaintClickDrawTool::Brush, gesture);
  assert(gesture_update.action == PaintClickDrawAction::None);
  gesture.escape = true;
  gesture_update = click_draw.Update(PaintClickDrawTool::Brush, gesture);
  assert(gesture_update.action == PaintClickDrawAction::StopBrush && !click_draw.Active());

  // A shape double-click fixes the start point; the next click provides its endpoint.
  PaintCanvas click_shape_canvas = MakePaintCanvas(12, 12, transparent);
  PaintHistory click_shape_history;
  PaintClickDrawState click_shape;
  gesture = {};
  gesture.double_click = true;
  gesture.click = true;
  gesture.inside = true;
  gesture.point = {1, 1};
  gesture_update = click_shape.Update(PaintClickDrawTool::Shape, gesture);
  assert(gesture_update.action == PaintClickDrawAction::PreviewShape && click_shape.ShapeActive());
  gesture = {};
  gesture.inside = true;
  gesture.point = {7, 7};
  gesture_update = click_shape.Update(PaintClickDrawTool::Shape, gesture);
  assert(gesture_update.action == PaintClickDrawAction::PreviewShape);
  gesture.click = true;
  gesture_update = click_shape.Update(PaintClickDrawTool::Shape, gesture);
  assert(gesture_update.action == PaintClickDrawAction::FinishShape && gesture_update.record_history);
  assert(gesture_update.from.x == 1 && gesture_update.from.y == 1);
  assert(gesture_update.to.x == 7 && gesture_update.to.y == 7);
  click_shape_history.Record(click_shape_canvas);
  assert(PaintLine(&click_shape_canvas, gesture_update.from, gesture_update.to, 1.0f, 1.0f, green));
  assert(click_shape_history.UndoCount() == 1 && click_shape_history.Undo(&click_shape_canvas));
  assert(Pixel(click_shape_canvas, 1, 1) == transparent && Pixel(click_shape_canvas, 7, 7) == transparent);

  assert(!MakePaintCanvas(257, 1).Valid());
  std::cout << "paint_editor_test ok\n";
  return 0;
}
