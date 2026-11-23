#include "internal/paint_editor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace wwe2k14::launcher::internal {
namespace {

size_t Offset(const PaintCanvas& canvas, int x, int y) {
  return (static_cast<size_t>(y) * static_cast<size_t>(canvas.width) + static_cast<size_t>(x)) * 4;
}

uint8_t Byte(float value) {
  return static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
}

bool BlendPixel(PaintCanvas* canvas, int x, int y, PaintColor color, uint8_t coverage, bool erase = false) {
  if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height || coverage == 0) return false;
  const size_t at = Offset(*canvas, x, y);
  if (erase) {
    const float strength = (static_cast<float>(coverage) * color.a) / (255.0f * 255.0f);
    const uint8_t alpha = Byte(canvas->rgba[at + 3] * (1.0f - strength));
    if (alpha == canvas->rgba[at + 3]) return false;
    canvas->rgba[at + 3] = alpha;
    if (alpha == 0) canvas->rgba[at] = canvas->rgba[at + 1] = canvas->rgba[at + 2] = 0;
    return true;
  }
  const float source_alpha = (static_cast<float>(coverage) * color.a) / 255.0f;
  if (source_alpha <= 0.0f) return false;
  const float destination_alpha = canvas->rgba[at + 3];
  const float output_alpha = source_alpha + destination_alpha * (255.0f - source_alpha) / 255.0f;
  if (output_alpha <= 0.0f) return false;
  const auto mix = [&](uint8_t source, uint8_t destination) {
    const float premultiplied = source * source_alpha +
                                destination * destination_alpha * (255.0f - source_alpha) / 255.0f;
    return Byte(premultiplied / output_alpha);
  };
  const PaintColor before{canvas->rgba[at], canvas->rgba[at + 1], canvas->rgba[at + 2], canvas->rgba[at + 3]};
  canvas->rgba[at] = mix(color.r, before.r);
  canvas->rgba[at + 1] = mix(color.g, before.g);
  canvas->rgba[at + 2] = mix(color.b, before.b);
  canvas->rgba[at + 3] = Byte(output_alpha);
  return canvas->rgba[at] != before.r || canvas->rgba[at + 1] != before.g ||
         canvas->rgba[at + 2] != before.b || canvas->rgba[at + 3] != before.a;
}

uint8_t BrushCoverage(float distance, float radius, float hardness) {
  if (distance > radius) return 0;
  const float inner = radius * hardness;
  if (distance <= inner || radius <= inner) return 255;
  return Byte(255.0f * (radius - distance) / (radius - inner));
}

void StampCoverage(const PaintCanvas& canvas, std::vector<uint8_t>* mask, float x, float y,
                   float size, float hardness) {
  const float radius = std::max(0.5f, size * 0.5f);
  hardness = std::clamp(hardness, 0.0f, 1.0f);
  const float cx = x + 0.5f, cy = y + 0.5f;
  const int left = std::max(0, static_cast<int>(std::floor(cx - radius - 0.5f)));
  const int right = std::min(canvas.width - 1, static_cast<int>(std::ceil(cx + radius - 0.5f)));
  const int top = std::max(0, static_cast<int>(std::floor(cy - radius - 0.5f)));
  const int bottom = std::min(canvas.height - 1, static_cast<int>(std::ceil(cy + radius - 0.5f)));
  for (int py = top; py <= bottom; ++py) {
    for (int px = left; px <= right; ++px) {
      const float dx = (px + 0.5f) - cx, dy = (py + 0.5f) - cy;
      const uint8_t coverage = BrushCoverage(std::sqrt(dx * dx + dy * dy), radius, hardness);
      if (!coverage) continue;
      const size_t index = static_cast<size_t>(py) * canvas.width + px;
      (*mask)[index] = std::max((*mask)[index], coverage);
    }
  }
}

bool ApplyCoverage(PaintCanvas* canvas, const std::vector<uint8_t>& mask, PaintColor color, bool erase = false) {
  bool changed = false;
  for (int y = 0; y < canvas->height; ++y) {
    for (int x = 0; x < canvas->width; ++x) {
      const uint8_t coverage = mask[static_cast<size_t>(y) * canvas->width + x];
      if (coverage) changed = BlendPixel(canvas, x, y, color, coverage, erase) || changed;
    }
  }
  return changed;
}

void RasterBrushLine(const PaintCanvas& canvas, std::vector<uint8_t>* mask, PaintPoint from, PaintPoint to,
                     float size, float hardness) {
  int x = from.x, y = from.y;
  const int dx = std::abs(to.x - from.x), sx = from.x < to.x ? 1 : -1;
  const int dy = -std::abs(to.y - from.y), sy = from.y < to.y ? 1 : -1;
  int error = dx + dy;
  for (;;) {
    StampCoverage(canvas, mask, static_cast<float>(x), static_cast<float>(y), size, hardness);
    if (x == to.x && y == to.y) break;
    const int twice = 2 * error;
    if (twice >= dy) { error += dy; x += sx; }
    if (twice <= dx) { error += dx; y += sy; }
  }
}

bool SetPixel(PaintCanvas* canvas, int x, int y, PaintColor color) {
  if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height) return false;
  const size_t at = Offset(*canvas, x, y);
  const PaintColor before{canvas->rgba[at], canvas->rgba[at + 1], canvas->rgba[at + 2], canvas->rgba[at + 3]};
  if (before == color) return false;
  canvas->rgba[at] = color.r; canvas->rgba[at + 1] = color.g;
  canvas->rgba[at + 2] = color.b; canvas->rgba[at + 3] = color.a;
  return true;
}

}  // namespace

bool PaintCanvas::Valid() const {
  return width > 0 && height > 0 && width <= 256 && height <= 256 &&
         rgba.size() == static_cast<size_t>(width) * height * 4;
}

PaintClickDrawUpdate PaintClickDrawState::Update(PaintClickDrawTool tool, const PaintClickDrawInput& input) {
  PaintClickDrawUpdate update;
  if (kind_ == Kind::None) {
    if (!input.double_click || !input.inside || tool == PaintClickDrawTool::None) return update;
    if (tool == PaintClickDrawTool::Brush) {
      kind_ = Kind::Brush;
      update.action = PaintClickDrawAction::StartBrush;
      update.record_history = !input.reuse_brush_checkpoint;
      update.from = input.reuse_brush_checkpoint ? input.brush_checkpoint : input.point;
      update.to = input.point;
      last_point_ = input.point;
      brush_reanchor_ = false;
    } else {
      kind_ = Kind::Shape;
      shape_start_ = input.point;
      shape_preview_ = input.point;
      update.action = PaintClickDrawAction::PreviewShape;
      update.from = shape_start_;
      update.to = shape_preview_;
    }
    return update;
  }

  if (input.escape) {
    update.action = kind_ == Kind::Brush ? PaintClickDrawAction::StopBrush : PaintClickDrawAction::CancelShape;
    kind_ = Kind::None;
    return update;
  }

  if (kind_ == Kind::Brush) {
    if (input.click) {
      if (input.inside && (input.point.x != last_point_.x || input.point.y != last_point_.y)) {
        update.action = PaintClickDrawAction::ExtendBrush;
        update.from = last_point_;
        update.to = input.point;
        update.stop_after = true;
      } else {
        update.action = PaintClickDrawAction::StopBrush;
      }
      kind_ = Kind::None;
      return update;
    }
    if (!input.inside) {
      last_point_ = input.point;
      brush_reanchor_ = true;
      return update;
    }
    if (brush_reanchor_) {
      last_point_ = input.point;
      brush_reanchor_ = false;
      return update;
    }
    if (input.inside && (input.point.x != last_point_.x || input.point.y != last_point_.y)) {
      update.action = PaintClickDrawAction::ExtendBrush;
      update.from = last_point_;
      update.to = input.point;
    }
    last_point_ = input.point;
    return update;
  }

  if (input.click) {
    if (input.inside) {
      update.action = PaintClickDrawAction::FinishShape;
      update.from = shape_start_;
      update.to = input.point;
      update.record_history = true;
    } else {
      update.action = PaintClickDrawAction::CancelShape;
    }
    kind_ = Kind::None;
    return update;
  }
  if (input.inside && (input.point.x != shape_preview_.x || input.point.y != shape_preview_.y)) {
    shape_preview_ = input.point;
    update.action = PaintClickDrawAction::PreviewShape;
    update.from = shape_start_;
    update.to = shape_preview_;
  }
  return update;
}

PaintCanvas MakePaintCanvas(int width, int height, PaintColor clear) {
  PaintCanvas canvas;
  if (width <= 0 || height <= 0 || width > 256 || height > 256) return canvas;
  canvas.width = width; canvas.height = height;
  canvas.rgba.resize(static_cast<size_t>(width) * height * 4);
  for (size_t i = 0; i < canvas.rgba.size(); i += 4) {
    canvas.rgba[i] = clear.r; canvas.rgba[i + 1] = clear.g;
    canvas.rgba[i + 2] = clear.b; canvas.rgba[i + 3] = clear.a;
  }
  return canvas;
}

bool ClearPaintCanvas(PaintCanvas* canvas, PaintColor color) {
  if (!canvas || !canvas->Valid()) return false;
  bool changed = false;
  for (size_t i = 0; i < canvas->rgba.size(); i += 4) {
    changed = canvas->rgba[i] != color.r || canvas->rgba[i + 1] != color.g ||
              canvas->rgba[i + 2] != color.b || canvas->rgba[i + 3] != color.a || changed;
    canvas->rgba[i] = color.r; canvas->rgba[i + 1] = color.g;
    canvas->rgba[i + 2] = color.b; canvas->rgba[i + 3] = color.a;
  }
  return changed;
}

bool PaintBrush(PaintCanvas* canvas, float x, float y, float size, float hardness, PaintColor color, bool erase) {
  if (!canvas || !canvas->Valid() || size <= 0.0f) return false;
  std::vector<uint8_t> mask(static_cast<size_t>(canvas->width) * canvas->height, 0);
  StampCoverage(*canvas, &mask, x, y, size, hardness);
  return ApplyCoverage(canvas, mask, color, erase);
}

bool PaintLine(PaintCanvas* canvas, PaintPoint from, PaintPoint to, float size, float hardness,
               PaintColor color, bool erase) {
  if (!canvas || !canvas->Valid() || size <= 0.0f) return false;
  std::vector<uint8_t> mask(static_cast<size_t>(canvas->width) * canvas->height, 0);
  RasterBrushLine(*canvas, &mask, from, to, size, hardness);
  return ApplyCoverage(canvas, mask, color, erase);
}

bool PaintRectangle(PaintCanvas* canvas, PaintPoint from, PaintPoint to, bool filled, float size,
                    float hardness, PaintColor color) {
  if (!canvas || !canvas->Valid()) return false;
  const int left = std::min(from.x, to.x), right = std::max(from.x, to.x);
  const int top = std::min(from.y, to.y), bottom = std::max(from.y, to.y);
  if (filled) {
    bool changed = false;
    for (int y = top; y <= bottom; ++y)
      for (int x = left; x <= right; ++x) changed = BlendPixel(canvas, x, y, color, 255) || changed;
    return changed;
  }
  if (size <= 0.0f) return false;
  std::vector<uint8_t> mask(static_cast<size_t>(canvas->width) * canvas->height, 0);
  RasterBrushLine(*canvas, &mask, {left, top}, {right, top}, size, hardness);
  RasterBrushLine(*canvas, &mask, {right, top}, {right, bottom}, size, hardness);
  RasterBrushLine(*canvas, &mask, {right, bottom}, {left, bottom}, size, hardness);
  RasterBrushLine(*canvas, &mask, {left, bottom}, {left, top}, size, hardness);
  return ApplyCoverage(canvas, mask, color);
}

bool PaintEllipse(PaintCanvas* canvas, PaintPoint from, PaintPoint to, bool filled, float size,
                  float hardness, PaintColor color) {
  if (!canvas || !canvas->Valid()) return false;
  const float left = static_cast<float>(std::min(from.x, to.x));
  const float right = static_cast<float>(std::max(from.x, to.x));
  const float top = static_cast<float>(std::min(from.y, to.y));
  const float bottom = static_cast<float>(std::max(from.y, to.y));
  const float cx = (left + right) * 0.5f, cy = (top + bottom) * 0.5f;
  const float rx = (right - left) * 0.5f, ry = (bottom - top) * 0.5f;
  if (rx < 0.5f) return PaintLine(canvas, {static_cast<int>(cx), static_cast<int>(top)},
                                         {static_cast<int>(cx), static_cast<int>(bottom)},
                                         filled ? 1.0f : size, hardness, color);
  if (ry < 0.5f) return PaintLine(canvas, {static_cast<int>(left), static_cast<int>(cy)},
                                         {static_cast<int>(right), static_cast<int>(cy)},
                                         filled ? 1.0f : size, hardness, color);
  if (filled) {
    bool changed = false;
    for (int y = static_cast<int>(std::floor(top)); y <= static_cast<int>(std::ceil(bottom)); ++y) {
      for (int x = static_cast<int>(std::floor(left)); x <= static_cast<int>(std::ceil(right)); ++x) {
        const float nx = (x - cx) / rx, ny = (y - cy) / ry;
        if (nx * nx + ny * ny <= 1.0f) changed = BlendPixel(canvas, x, y, color, 255) || changed;
      }
    }
    return changed;
  }
  if (size <= 0.0f) return false;
  std::vector<uint8_t> mask(static_cast<size_t>(canvas->width) * canvas->height, 0);
  const int steps = std::max(16, static_cast<int>(std::ceil(2.0f * 3.14159265f * std::max(rx, ry) * 2.0f)));
  PaintPoint previous{static_cast<int>(std::lround(cx + rx)), static_cast<int>(std::lround(cy))};
  for (int i = 1; i <= steps; ++i) {
    const float angle = 2.0f * 3.14159265f * i / steps;
    const PaintPoint next{static_cast<int>(std::lround(cx + std::cos(angle) * rx)),
                          static_cast<int>(std::lround(cy + std::sin(angle) * ry))};
    RasterBrushLine(*canvas, &mask, previous, next, size, hardness);
    previous = next;
  }
  return ApplyCoverage(canvas, mask, color);
}

bool PaintFill(PaintCanvas* canvas, int x, int y, PaintColor color) {
  if (!canvas || !canvas->Valid() || x < 0 || y < 0 || x >= canvas->width || y >= canvas->height) return false;
  const PaintColor target = PickPaintColor(*canvas, x, y);
  if (target == color) return false;
  const size_t count = static_cast<size_t>(canvas->width) * canvas->height;
  std::vector<uint8_t> seen(count, 0);
  std::vector<size_t> queue;
  queue.reserve(count);
  const size_t start = static_cast<size_t>(y) * canvas->width + x;
  queue.push_back(start); seen[start] = 1;
  for (size_t cursor = 0; cursor < queue.size(); ++cursor) {
    const size_t index = queue[cursor];
    const int px = static_cast<int>(index % canvas->width), py = static_cast<int>(index / canvas->width);
    if (!(PickPaintColor(*canvas, px, py) == target)) continue;
    SetPixel(canvas, px, py, color);
    const auto add = [&](int nx, int ny) {
      if (nx < 0 || ny < 0 || nx >= canvas->width || ny >= canvas->height) return;
      const size_t next = static_cast<size_t>(ny) * canvas->width + nx;
      if (!seen[next]) { seen[next] = 1; queue.push_back(next); }
    };
    add(px - 1, py); add(px + 1, py); add(px, py - 1); add(px, py + 1);
  }
  return true;
}

PaintColor PickPaintColor(const PaintCanvas& canvas, int x, int y) {
  if (!canvas.Valid() || x < 0 || y < 0 || x >= canvas.width || y >= canvas.height) return {0, 0, 0, 0};
  const size_t at = Offset(canvas, x, y);
  return {canvas.rgba[at], canvas.rgba[at + 1], canvas.rgba[at + 2], canvas.rgba[at + 3]};
}

bool PaintMask(PaintCanvas* canvas, int x, int y, int mask_width, int mask_height,
               const std::vector<uint8_t>& alpha_mask, PaintColor color) {
  if (!canvas || !canvas->Valid() || mask_width <= 0 || mask_height <= 0 ||
      alpha_mask.size() != static_cast<size_t>(mask_width) * mask_height) return false;
  bool changed = false;
  for (int my = 0; my < mask_height; ++my) {
    const int cy = y + my;
    if (cy < 0 || cy >= canvas->height) continue;
    for (int mx = 0; mx < mask_width; ++mx) {
      const int cx = x + mx;
      if (cx < 0 || cx >= canvas->width) continue;
      changed = BlendPixel(canvas, cx, cy, color, alpha_mask[static_cast<size_t>(my) * mask_width + mx]) || changed;
    }
  }
  return changed;
}

bool CompositePaintImage(PaintCanvas* destination, const PaintCanvas& source, int x, int y,
                         int width, int height) {
  if (!destination || !destination->Valid() || !source.Valid() || width <= 0 || height <= 0) return false;
  bool changed = false;
  for (int dy = 0; dy < height; ++dy) {
    const int target_y = y + dy;
    if (target_y < 0 || target_y >= destination->height) continue;
    const int source_y = std::min(source.height - 1, static_cast<int>((static_cast<int64_t>(dy) * source.height) / height));
    for (int dx = 0; dx < width; ++dx) {
      const int target_x = x + dx;
      if (target_x < 0 || target_x >= destination->width) continue;
      const int source_x = std::min(source.width - 1, static_cast<int>((static_cast<int64_t>(dx) * source.width) / width));
      const size_t at = Offset(source, source_x, source_y);
      const PaintColor color{source.rgba[at], source.rgba[at + 1], source.rgba[at + 2], source.rgba[at + 3]};
      if (color.a) changed = BlendPixel(destination, target_x, target_y, color, 255) || changed;
    }
  }
  return changed;
}

PaintHistory::PaintHistory(size_t capacity) : capacity_(std::max<size_t>(1, capacity)) {}

void PaintHistory::Push(std::vector<std::vector<uint8_t>>* stack, const std::vector<uint8_t>& pixels) {
  if (capacity_ == 0) return;
  stack->push_back(pixels);
  if (stack->size() > capacity_) stack->erase(stack->begin());
}

void PaintHistory::Record(const PaintCanvas& canvas) {
  if (!canvas.Valid()) return;
  Push(&undo_, canvas.rgba);
  redo_.clear();
}

bool PaintHistory::Undo(PaintCanvas* canvas) {
  if (!canvas || !canvas->Valid() || undo_.empty() || undo_.back().size() != canvas->rgba.size()) return false;
  Push(&redo_, canvas->rgba);
  canvas->rgba = std::move(undo_.back());
  undo_.pop_back();
  return true;
}

bool PaintHistory::Redo(PaintCanvas* canvas) {
  if (!canvas || !canvas->Valid() || redo_.empty() || redo_.back().size() != canvas->rgba.size()) return false;
  Push(&undo_, canvas->rgba);
  canvas->rgba = std::move(redo_.back());
  redo_.pop_back();
  return true;
}

void PaintHistory::Clear() {
  undo_.clear(); redo_.clear();
}

}  // namespace wwe2k14::launcher::internal
