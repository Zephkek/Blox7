#include "runtime/roblox_text_editor.h"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <utf8proc.h>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace blox7 {
namespace runtime {
namespace {

constexpr std::size_t kMaximumTextBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaximumPendingNativeEchoes = 128;
constexpr uint32_t kShortcutSelectAll = 1U << 0U;
constexpr uint32_t kShortcutCopy = 1U << 1U;
constexpr uint32_t kShortcutPaste = 1U << 2U;
constexpr uint32_t kShortcutCut = 1U << 3U;

Status InvalidArgument(const char* message) {
  return Status::Error(StatusCode::kInvalidArgument, message);
}

Status FailedPrecondition(const char* message) {
  return Status::Error(StatusCode::kFailedPrecondition, message);
}

Status Unsupported(const char* message) {
  return Status::Error(StatusCode::kUnsupported, message);
}

struct Utf8Layout {
  bool valid = false;
  int32_t utf16_units = 0;
  std::vector<std::size_t> grapheme_boundaries;
};

Utf8Layout Layout(const std::string& text) {
  Utf8Layout layout;
  layout.grapheme_boundaries.push_back(0);
  utf8proc_int32_t previous = 0;
  utf8proc_int32_t break_state = 0;
  bool have_previous = false;
  std::size_t offset = 0;
  int64_t utf16_units = 0;
  while (offset < text.size()) {
    utf8proc_int32_t codepoint = 0;
    const utf8proc_ssize_t count = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(text.data() + offset),
        static_cast<utf8proc_ssize_t>(text.size() - offset), &codepoint);
    if (count <= 0) {
      return layout;
    }
    if (have_previous &&
        utf8proc_grapheme_break_stateful(previous, codepoint, &break_state)) {
      layout.grapheme_boundaries.push_back(offset);
    }
    utf16_units += codepoint > 0xffff ? 2 : 1;
    if (utf16_units > std::numeric_limits<int32_t>::max()) {
      return layout;
    }
    previous = codepoint;
    have_previous = true;
    offset += static_cast<std::size_t>(count);
  }
  if (layout.grapheme_boundaries.back() != text.size()) {
    layout.grapheme_boundaries.push_back(text.size());
  }
  layout.valid = true;
  layout.utf16_units = static_cast<int32_t>(utf16_units);
  return layout;
}

int32_t Utf16Cursor(const std::string& text, std::size_t byte_offset) {
  std::size_t offset = 0;
  int32_t units = 0;
  while (offset < byte_offset) {
    utf8proc_int32_t codepoint = 0;
    const utf8proc_ssize_t count = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(text.data() + offset),
        static_cast<utf8proc_ssize_t>(byte_offset - offset), &codepoint);
    if (count <= 0) {
      return 0;
    }
    units += codepoint > 0xffff ? 2 : 1;
    offset += static_cast<std::size_t>(count);
  }
  return units;
}

std::size_t Utf8CharacterByteOffset(const std::string& text,
                                    int character_index) {
  if (character_index < 0) {
    return text.size();
  }
  std::size_t offset = 0;
  for (int index = 0; index < character_index && offset < text.size();
       ++index) {
    utf8proc_int32_t codepoint = 0;
    const utf8proc_ssize_t count = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(text.data() + offset),
        static_cast<utf8proc_ssize_t>(text.size() - offset), &codepoint);
    if (count <= 0) {
      return text.size();
    }
    offset += static_cast<std::size_t>(count);
  }
  return offset;
}

enum class CharacterClass { kSpace, kWord, kOther };

CharacterClass ClassAt(const std::string& text, std::size_t byte_offset) {
  utf8proc_int32_t codepoint = 0;
  if (byte_offset >= text.size() ||
      utf8proc_iterate(
          reinterpret_cast<const utf8proc_uint8_t*>(text.data() + byte_offset),
          static_cast<utf8proc_ssize_t>(text.size() - byte_offset),
          &codepoint) <= 0) {
    return CharacterClass::kOther;
  }
  if (codepoint == '_') {
    return CharacterClass::kWord;
  }
  switch (utf8proc_category(codepoint)) {
    case UTF8PROC_CATEGORY_LU:
    case UTF8PROC_CATEGORY_LL:
    case UTF8PROC_CATEGORY_LT:
    case UTF8PROC_CATEGORY_LM:
    case UTF8PROC_CATEGORY_LO:
    case UTF8PROC_CATEGORY_MN:
    case UTF8PROC_CATEGORY_MC:
    case UTF8PROC_CATEGORY_ME:
    case UTF8PROC_CATEGORY_ND:
    case UTF8PROC_CATEGORY_NL:
    case UTF8PROC_CATEGORY_NO:
      return CharacterClass::kWord;
    case UTF8PROC_CATEGORY_ZS:
    case UTF8PROC_CATEGORY_ZL:
    case UTF8PROC_CATEGORY_ZP:
      return CharacterClass::kSpace;
    case UTF8PROC_CATEGORY_CC:
      return codepoint == '\t' || codepoint == '\n' || codepoint == '\r'
                 ? CharacterClass::kSpace
                 : CharacterClass::kOther;
    default:
      return CharacterClass::kOther;
  }
}

std::size_t CaretIndex(const std::vector<std::size_t>& boundaries,
                       std::size_t byte_offset) {
  const auto at = std::lower_bound(boundaries.begin(), boundaries.end(),
                                   byte_offset);
  return std::min(static_cast<std::size_t>(at - boundaries.begin()),
                  boundaries.size() - 1);
}

std::size_t WordLeft(const std::string& text,
                     const std::vector<std::size_t>& boundaries,
                     std::size_t byte_offset) {
  std::size_t index = CaretIndex(boundaries, byte_offset);
  while (index > 0 && ClassAt(text, boundaries[index - 1]) ==
                          CharacterClass::kSpace) {
    --index;
  }
  if (index == 0) {
    return 0;
  }
  const CharacterClass run = ClassAt(text, boundaries[index - 1]);
  while (index > 0 && ClassAt(text, boundaries[index - 1]) == run) {
    --index;
  }
  return boundaries[index];
}

std::size_t WordRight(const std::string& text,
                      const std::vector<std::size_t>& boundaries,
                      std::size_t byte_offset) {
  std::size_t index = CaretIndex(boundaries, byte_offset);
  const std::size_t last = boundaries.size() - 1;
  if (index >= last) {
    return text.size();
  }
  const CharacterClass run = ClassAt(text, boundaries[index]);
  if (run != CharacterClass::kSpace) {
    while (index < last && ClassAt(text, boundaries[index]) == run) {
      ++index;
    }
  }
  while (index < last &&
         ClassAt(text, boundaries[index]) == CharacterClass::kSpace) {
    ++index;
  }
  return boundaries[index];
}

std::pair<std::size_t, std::size_t> WordAt(
    const std::string& text, const std::vector<std::size_t>& boundaries,
    std::size_t byte_offset) {
  const std::size_t last = boundaries.size() - 1;
  if (last == 0) {
    return {0, 0};
  }
  const std::size_t caret = CaretIndex(boundaries, byte_offset);
  std::size_t index = caret < last ? caret : last - 1;
  if (caret > 0 &&
      (caret == last ||
       ClassAt(text, boundaries[caret]) != CharacterClass::kWord) &&
      ClassAt(text, boundaries[caret - 1]) == CharacterClass::kWord) {
    index = caret - 1;
  }
  const CharacterClass run = ClassAt(text, boundaries[index]);
  if (run == CharacterClass::kOther) {
    return {boundaries[index], boundaries[index + 1]};
  }
  std::size_t begin = index;
  while (begin > 0 && ClassAt(text, boundaries[begin - 1]) == run) {
    --begin;
  }
  std::size_t end = index + 1;
  while (end < last && ClassAt(text, boundaries[end]) == run) {
    ++end;
  }
  return {boundaries[begin], boundaries[end]};
}

RobloxTextEditResult Result(bool handled, bool dispatched, bool ended,
                            Status status = Status::Ok()) {
  return {handled, dispatched, ended, std::move(status)};
}

void SecureClear(std::string* text) {
  if (text == nullptr) {
    return;
  }
  volatile char* bytes = text->empty() ? nullptr : &(*text)[0];
  for (std::size_t index = 0; index < text->size(); ++index) {
    bytes[index] = 0;
  }
  std::string empty;
  text->swap(empty);
}

uint32_t ShortcutBit(SDL_Scancode scancode) {
  switch (scancode) {
    case SDL_SCANCODE_A:
      return kShortcutSelectAll;
    case SDL_SCANCODE_C:
      return kShortcutCopy;
    case SDL_SCANCODE_V:
      return kShortcutPaste;
    case SDL_SCANCODE_X:
      return kShortcutCut;
    default:
      return 0;
  }
}

bool IsPasswordInputType(int32_t text_input_type) {
  return text_input_type == 5 || text_input_type == 9 ||
         text_input_type == 10;
}

}  // namespace

RobloxTextEditor::RobloxTextEditor(RobloxTextSink sink,
                                   RobloxTextDisplaySink display_sink,
                                   platform::TextClipboard* clipboard)
    : sink_(sink), display_sink_(display_sink), clipboard_(clipboard) {}

Status RobloxTextEditor::BeginFocusSession(RobloxTextFocusSession session) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (session.textbox_handle == 0 || session.generation == 0) {
    return InvalidArgument("TextBox handle and generation must be non-zero");
  }
  if (session.initial_text.size() > kMaximumTextBytes ||
      !Layout(session.initial_text).valid) {
    return InvalidArgument(
        "initial TextBox text is invalid UTF-8 or too large");
  }
  if (sink_.sync == nullptr || sink_.pass_text == nullptr ||
      sink_.return_pressed == nullptr || sink_.release_focus == nullptr) {
    return Unsupported("Roblox TextBox native export table is incomplete");
  }
  if (snapshot_.focused) {
    ClearLocked();
  }
  text_ = std::move(session.initial_text);
  session.initial_text.clear();
  session_ = std::move(session);
  cursor_byte_ = text_.size();
  selection_anchor_byte_ = cursor_byte_;
  composition_begin_byte_ = 0;
  composition_end_byte_ = 0;
  composition_original_cursor_byte_ = 0;
  composition_original_anchor_byte_ = 0;
  SecureClear(&composition_replaced_text_);
  composition_active_ = false;
  active_shortcuts_ = 0;
  snapshot_.focused = true;
  snapshot_.textbox_handle = session_.textbox_handle;
  snapshot_.generation = session_.generation;
  snapshot_.text_bytes = text_.size();
  UpdateSelectionSnapshotLocked();
  PublishDisplayLocked(RobloxTextDisplayEvent::kShow);
  return Status::Ok();
}

Status RobloxTextEditor::EndFocusSession(int64_t textbox_handle,
                                         uint64_t generation,
                                         bool notify_native) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused) {
    return Status::Ok();
  }
  if (textbox_handle != session_.textbox_handle ||
      generation != session_.generation) {
    return FailedPrecondition("stale Roblox TextBox focus generation");
  }
  Status status = Status::Ok();
  if (notify_native) {
    status = sink_.release_focus(sink_.context, session_.textbox_handle);
    if (!status.ok()) {
      ++snapshot_.native_errors;
    }
  }
  ClearLocked();
  return status;
}

Status RobloxTextEditor::ReplaceFocusedTextFromEngine(
    uint64_t generation, std::string authoritative_utf8) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused || generation != session_.generation) {
    SecureClear(&authoritative_utf8);
    return FailedPrecondition("stale Roblox TextBox engine update");
  }
  if (authoritative_utf8.size() > kMaximumTextBytes ||
      !Layout(authoritative_utf8).valid) {
    SecureClear(&authoritative_utf8);
    return InvalidArgument("engine TextBox text is invalid UTF-8 or too large");
  }
  if (authoritative_utf8.empty()) {
    ClearPendingNativeEchoesLocked();
    SecureClear(&text_);
    cursor_byte_ = 0;
    selection_anchor_byte_ = 0;
    composition_begin_byte_ = 0;
    composition_end_byte_ = 0;
    composition_original_cursor_byte_ = 0;
    composition_original_anchor_byte_ = 0;
    SecureClear(&composition_replaced_text_);
    composition_active_ = false;
    snapshot_.text_bytes = 0;
    UpdateSelectionSnapshotLocked();
    PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
    return Status::Ok();
  }
  if (ConsumePendingNativeEchoLocked(authoritative_utf8)) {
    SecureClear(&authoritative_utf8);
    return Status::Ok();
  }
  if (authoritative_utf8 == text_) {
    SecureClear(&authoritative_utf8);
    return Status::Ok();
  }
  SecureClear(&text_);
  text_ = std::move(authoritative_utf8);
  cursor_byte_ = text_.size();
  selection_anchor_byte_ = cursor_byte_;
  composition_begin_byte_ = 0;
  composition_end_byte_ = 0;
  composition_original_cursor_byte_ = 0;
  composition_original_anchor_byte_ = 0;
  SecureClear(&composition_replaced_text_);
  composition_active_ = false;
  snapshot_.text_bytes = text_.size();
  UpdateSelectionSnapshotLocked();
  PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
  return Status::Ok();
}

Status RobloxTextEditor::UpdateFocusProperties(
    uint64_t generation, const RobloxTextFocusProperties& properties) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused || generation != session_.generation) {
    return FailedPrecondition("stale Roblox TextBox property update");
  }
  session_.manual_focus_release = properties.manual_focus_release;
  session_.multiline = properties.multiline;
  session_.area_x = properties.area_x;
  session_.area_y = properties.area_y;
  session_.area_width = properties.area_width;
  session_.area_height = properties.area_height;
  session_.font_size = properties.font_size;
  session_.x_alignment = properties.x_alignment;
  session_.y_alignment = properties.y_alignment;
  session_.text_color = properties.text_color;
  session_.font = properties.font;
  session_.text_input_type = properties.text_input_type;
  session_.return_key_type = properties.return_key_type;
  session_.text_wrapped = properties.text_wrapped;
  PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
  return Status::Ok();
}

RobloxTextEditResult RobloxTextEditor::HandleTextInput(
    const platform::TextInputEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused) {
    return Result(false, false, false);
  }
  if (event.text.empty()) {
    return Result(true, false, false);
  }
  const std::size_t replace_begin =
      composition_active_ ? composition_begin_byte_ : SelectionBeginLocked();
  const std::size_t replace_end =
      composition_active_ ? composition_end_byte_ : SelectionEndLocked();
  if (!Layout(event.text).valid ||
      event.text.size() >
          kMaximumTextBytes - (text_.size() - (replace_end - replace_begin))) {
    return Result(
        true, false, false,
        InvalidArgument("committed text is invalid UTF-8 or too large"));
  }
  if (composition_active_) {
    composition_active_ = false;
    SecureClear(&composition_replaced_text_);
    return ReplaceLocked(replace_begin, replace_end, event.text);
  }
  return ReplaceLocked(replace_begin, replace_end, event.text);
}

RobloxTextEditResult RobloxTextEditor::HandleTextEditing(
    const platform::TextEditingEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused) {
    return Result(false, false, false);
  }
  return ReplaceCompositionLocked(event);
}

RobloxTextEditResult RobloxTextEditor::HandleKey(
    const platform::KeyEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused) {
    return Result(false, false, false);
  }

  const auto scancode = static_cast<SDL_Scancode>(event.scancode);
  const uint32_t shortcut_bit = ShortcutBit(scancode);
  const bool ctrl_pressed =
      (event.modifiers & static_cast<uint32_t>(SDL_KMOD_CTRL)) != 0;
  const bool alt_pressed =
      (event.modifiers & static_cast<uint32_t>(SDL_KMOD_ALT)) != 0;
  const bool shift_pressed =
      (event.modifiers & static_cast<uint32_t>(SDL_KMOD_SHIFT)) != 0;
  const bool shortcut = shortcut_bit != 0 && ctrl_pressed && !alt_pressed;
  const bool edit_key =
      scancode == SDL_SCANCODE_BACKSPACE || scancode == SDL_SCANCODE_DELETE ||
      scancode == SDL_SCANCODE_LEFT || scancode == SDL_SCANCODE_RIGHT ||
      scancode == SDL_SCANCODE_HOME || scancode == SDL_SCANCODE_END ||
      scancode == SDL_SCANCODE_RETURN || scancode == SDL_SCANCODE_KP_ENTER ||
      scancode == SDL_SCANCODE_ESCAPE || shortcut;
  if (!event.pressed && shortcut_bit != 0 &&
      (active_shortcuts_ & shortcut_bit) != 0) {
    active_shortcuts_ &= ~shortcut_bit;
    return Result(true, false, false);
  }
  if (!edit_key) {
    return Result(false, false, false);
  }
  if (!event.pressed) {
    return Result(true, false, false);
  }
  if (shortcut) {
    active_shortcuts_ |= shortcut_bit;
  }

  if (composition_active_) {
    return Result(true, false, false);
  }

  if (shortcut_bit == kShortcutSelectAll) {
    return SelectAllLocked();
  }
  if (shortcut_bit == kShortcutCopy) {
    return CopySelectionLocked(false);
  }
  if (shortcut_bit == kShortcutPaste) {
    return PasteLocked();
  }
  if (shortcut_bit == kShortcutCut) {
    return CopySelectionLocked(true);
  }

  const Utf8Layout layout = Layout(text_);
  const auto current =
      std::lower_bound(layout.grapheme_boundaries.begin(),
                       layout.grapheme_boundaries.end(), cursor_byte_);
  if (ctrl_pressed && !alt_pressed && layout.valid &&
      (scancode == SDL_SCANCODE_LEFT || scancode == SDL_SCANCODE_RIGHT ||
       scancode == SDL_SCANCODE_BACKSPACE ||
       scancode == SDL_SCANCODE_DELETE)) {
    const bool password = IsPasswordInputType(session_.text_input_type);
    const std::size_t left =
        password ? 0
                 : WordLeft(text_, layout.grapheme_boundaries, cursor_byte_);
    const std::size_t right =
        password ? text_.size()
                 : WordRight(text_, layout.grapheme_boundaries, cursor_byte_);
    if (scancode == SDL_SCANCODE_LEFT) {
      return MoveCursorLocked(left, shift_pressed);
    }
    if (scancode == SDL_SCANCODE_RIGHT) {
      return MoveCursorLocked(right, shift_pressed);
    }
    if (HasSelectionLocked()) {
      return ReplaceLocked(SelectionBeginLocked(), SelectionEndLocked(), "");
    }
    if (scancode == SDL_SCANCODE_BACKSPACE) {
      return left == cursor_byte_ ? Result(true, false, false)
                                  : ReplaceLocked(left, cursor_byte_, "");
    }
    return right == cursor_byte_ ? Result(true, false, false)
                                 : ReplaceLocked(cursor_byte_, right, "");
  }
  if (scancode == SDL_SCANCODE_BACKSPACE) {
    if (HasSelectionLocked()) {
      return ReplaceLocked(SelectionBeginLocked(), SelectionEndLocked(), "");
    }
    if (current == layout.grapheme_boundaries.begin()) {
      return Result(true, false, false);
    }
    return ReplaceLocked(*(current - 1), cursor_byte_, "");
  }
  if (scancode == SDL_SCANCODE_DELETE) {
    if (HasSelectionLocked()) {
      return ReplaceLocked(SelectionBeginLocked(), SelectionEndLocked(), "");
    }
    if (current == layout.grapheme_boundaries.end() ||
        current + 1 == layout.grapheme_boundaries.end()) {
      return Result(true, false, false);
    }
    return ReplaceLocked(cursor_byte_, *(current + 1), "");
  }
  if (scancode == SDL_SCANCODE_LEFT) {
    if (HasSelectionLocked() && !shift_pressed) {
      return MoveCursorLocked(SelectionBeginLocked(), false);
    }
    return MoveCursorLocked(current == layout.grapheme_boundaries.begin()
                                ? cursor_byte_
                                : *(current - 1),
                            shift_pressed);
  }
  if (scancode == SDL_SCANCODE_RIGHT) {
    if (HasSelectionLocked() && !shift_pressed) {
      return MoveCursorLocked(SelectionEndLocked(), false);
    }
    return MoveCursorLocked(current + 1 == layout.grapheme_boundaries.end()
                                ? cursor_byte_
                                : *(current + 1),
                            shift_pressed);
  }
  if (scancode == SDL_SCANCODE_HOME) {
    return MoveCursorLocked(0, shift_pressed);
  }
  if (scancode == SDL_SCANCODE_END) {
    return MoveCursorLocked(text_.size(), shift_pressed);
  }
  if (scancode == SDL_SCANCODE_ESCAPE) {
    return ReleaseLocked();
  }
  if (session_.multiline) {
    return ReplaceLocked(SelectionBeginLocked(), SelectionEndLocked(), "\n");
  }
  return FinishLocked();
}

RobloxTextEditResult RobloxTextEditor::HandlePointerButton(float x, float y,
                                                           bool pressed,
                                                           uint8_t clicks,
                                                           bool extend) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused) {
    pointer_drag_ = false;
    return Result(false, false, false);
  }
  if (!pressed) {
    const bool dragging = pointer_drag_;
    pointer_drag_ = false;
    return Result(dragging, false, false);
  }
  std::size_t byte = 0;
  if (!PointerByteLocked(x, y, false, &byte)) {
    return Result(false, false, false);
  }
  pointer_drag_ = true;
  if (composition_active_) {
    return Result(true, false, false);
  }
  if (clicks >= 3 ||
      (clicks == 2 && IsPasswordInputType(session_.text_input_type))) {
    return SelectAllLocked();
  }
  if (clicks == 2) {
    const Utf8Layout layout = Layout(text_);
    if (!layout.valid) {
      return Result(true, false, false);
    }
    const auto word = WordAt(text_, layout.grapheme_boundaries, byte);
    return SelectRangeLocked(word.first, word.second);
  }
  return MoveCursorLocked(byte, extend);
}

RobloxTextEditResult RobloxTextEditor::HandlePointerMotion(float x, float y) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused || !pointer_drag_ || composition_active_) {
    return Result(false, false, false);
  }
  std::size_t byte = 0;
  if (!PointerByteLocked(x, y, true, &byte)) {
    return Result(false, false, false);
  }
  return MoveCursorLocked(byte, true);
}

bool RobloxTextEditor::PointerByteLocked(float x, float y, bool clamp,
                                         std::size_t* byte) const {
  std::size_t code_points = 0;
  if (display_sink_.hit_test == nullptr ||
      !display_sink_.hit_test(display_sink_.context, x, y, clamp,
                              &code_points)) {
    return false;
  }
  const std::size_t offset = Utf8CharacterByteOffset(
      text_, static_cast<int>(std::min<std::size_t>(
                 code_points, std::numeric_limits<int>::max())));
  const Utf8Layout layout = Layout(text_);
  const auto boundary =
      std::lower_bound(layout.grapheme_boundaries.begin(),
                       layout.grapheme_boundaries.end(), offset);
  *byte = boundary == layout.grapheme_boundaries.end() ? text_.size()
                                                       : *boundary;
  return true;
}

RobloxTextEditorSnapshot RobloxTextEditor::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

RobloxTextEditResult RobloxTextEditor::ReplaceLocked(
    std::size_t begin, std::size_t end, const std::string& replacement) {
  std::string updated;
  updated.reserve(text_.size() - (end - begin) + replacement.size());
  updated.append(text_, 0, begin);
  updated.append(replacement);
  updated.append(text_, end, std::string::npos);
  SecureClear(&text_);
  text_ = std::move(updated);
  cursor_byte_ = begin + replacement.size();
  selection_anchor_byte_ = cursor_byte_;
  composition_begin_byte_ = 0;
  composition_end_byte_ = 0;
  composition_original_cursor_byte_ = 0;
  composition_original_anchor_byte_ = 0;
  SecureClear(&composition_replaced_text_);
  composition_active_ = false;
  snapshot_.text_bytes = text_.size();
  UpdateSelectionSnapshotLocked();
  Status status = SyncLocked();
  if (status.ok()) {
    status = PassLocked(false);
  }
  if (status.ok()) {
    RememberPendingNativeEchoLocked();
    ++snapshot_.committed_edits;
    PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
  } else {
    ++snapshot_.native_errors;
    sink_.release_focus(sink_.context, session_.textbox_handle);
    ClearLocked();
  }
  const bool ok = status.ok();
  return Result(true, ok, !ok, std::move(status));
}

RobloxTextEditResult RobloxTextEditor::ReplaceCompositionLocked(
    const platform::TextEditingEvent& event) {
  if (!composition_active_ && event.text.empty()) {
    return Result(true, false, false);
  }
  const std::size_t begin =
      composition_active_ ? composition_begin_byte_ : SelectionBeginLocked();
  const std::size_t end =
      composition_active_ ? composition_end_byte_ : SelectionEndLocked();
  if (!Layout(event.text).valid ||
      event.text.size() > kMaximumTextBytes - (text_.size() - (end - begin))) {
    return Result(
        true, false, false,
        InvalidArgument("preedit text is invalid UTF-8 or too large"));
  }

  if (!composition_active_) {
    composition_original_cursor_byte_ = cursor_byte_;
    composition_original_anchor_byte_ = selection_anchor_byte_;
    composition_replaced_text_.assign(text_, begin, end - begin);
  }
  const bool cancel_composition = event.text.empty();
  const std::string& visible_replacement =
      cancel_composition ? composition_replaced_text_ : event.text;

  std::string updated;
  updated.reserve(text_.size() - (end - begin) + visible_replacement.size());
  updated.append(text_, 0, begin);
  updated.append(visible_replacement);
  updated.append(text_, end, std::string::npos);
  SecureClear(&text_);
  text_ = std::move(updated);
  composition_begin_byte_ = begin;
  composition_end_byte_ = begin + visible_replacement.size();
  composition_active_ = !cancel_composition;
  if (cancel_composition) {
    cursor_byte_ = composition_original_cursor_byte_;
    selection_anchor_byte_ = composition_original_anchor_byte_;
    composition_original_cursor_byte_ = 0;
    composition_original_anchor_byte_ = 0;
    SecureClear(&composition_replaced_text_);
  } else {
    cursor_byte_ = begin + Utf8CharacterByteOffset(event.text, event.start);
    selection_anchor_byte_ = cursor_byte_;
  }
  snapshot_.text_bytes = text_.size();
  UpdateSelectionSnapshotLocked();

  Status status = SyncLocked();
  if (status.ok()) {
    status = PassLocked(false);
  }
  if (status.ok()) {
    RememberPendingNativeEchoLocked();
    PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
  } else {
    ++snapshot_.native_errors;
    sink_.release_focus(sink_.context, session_.textbox_handle);
    ClearLocked();
  }
  const bool ok = status.ok();
  return Result(true, ok, !ok, std::move(status));
}

RobloxTextEditResult RobloxTextEditor::MoveCursorLocked(
    std::size_t byte_offset, bool extend_selection) {
  const std::size_t previous_cursor = cursor_byte_;
  const std::size_t previous_anchor = selection_anchor_byte_;
  cursor_byte_ = byte_offset;
  if (!extend_selection) {
    selection_anchor_byte_ = cursor_byte_;
  }
  if (cursor_byte_ == previous_cursor &&
      selection_anchor_byte_ == previous_anchor) {
    return Result(true, false, false);
  }
  UpdateSelectionSnapshotLocked();
  if (cursor_byte_ == previous_cursor) {
    PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
    return Result(true, false, false);
  }
  Status status = SyncLocked();
  if (!status.ok()) {
    ++snapshot_.native_errors;
    sink_.release_focus(sink_.context, session_.textbox_handle);
    ClearLocked();
  } else {
    PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
  }
  const bool ok = status.ok();
  return Result(true, ok, !ok, std::move(status));
}

RobloxTextEditResult RobloxTextEditor::SelectAllLocked() {
  return SelectRangeLocked(0, text_.size());
}

RobloxTextEditResult RobloxTextEditor::SelectRangeLocked(
    std::size_t anchor_byte, std::size_t cursor_byte) {
  const std::size_t previous_cursor = cursor_byte_;
  const std::size_t previous_anchor = selection_anchor_byte_;
  selection_anchor_byte_ = anchor_byte;
  cursor_byte_ = cursor_byte;
  if (cursor_byte_ == previous_cursor &&
      selection_anchor_byte_ == previous_anchor) {
    return Result(true, false, false);
  }
  UpdateSelectionSnapshotLocked();
  Status status = Status::Ok();
  const bool cursor_changed = cursor_byte_ != previous_cursor;
  if (cursor_changed) {
    status = SyncLocked();
  }
  if (!status.ok()) {
    ++snapshot_.native_errors;
    sink_.release_focus(sink_.context, session_.textbox_handle);
    ClearLocked();
  } else {
    PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
  }
  const bool ok = status.ok();
  return Result(true, cursor_changed && ok, !ok, std::move(status));
}

RobloxTextEditResult RobloxTextEditor::CopySelectionLocked(bool cut) {
  if (!HasSelectionLocked() || clipboard_ == nullptr ||
      IsPasswordInputType(session_.text_input_type)) {
    return Result(true, false, false);
  }
  const std::size_t begin = SelectionBeginLocked();
  const std::size_t end = SelectionEndLocked();
  std::string selected(text_, begin, end - begin);
  const Status clipboard_status = clipboard_->WriteText(selected);
  SecureClear(&selected);
  if (!clipboard_status.ok() || !cut) {
    return Result(true, false, false);
  }
  return ReplaceLocked(begin, end, "");
}

RobloxTextEditResult RobloxTextEditor::PasteLocked() {
  if (clipboard_ == nullptr) {
    return Result(true, false, false);
  }
  std::string clipboard_text;
  const Status clipboard_status = clipboard_->ReadText(&clipboard_text);
  const std::size_t begin = SelectionBeginLocked();
  const std::size_t end = SelectionEndLocked();
  const bool valid = clipboard_status.ok() && !clipboard_text.empty() &&
                     clipboard_text.size() <=
                         kMaximumTextBytes - (text_.size() - (end - begin)) &&
                     Layout(clipboard_text).valid;
  if (!valid) {
    SecureClear(&clipboard_text);
    return Result(true, false, false);
  }
  RobloxTextEditResult result = ReplaceLocked(begin, end, clipboard_text);
  SecureClear(&clipboard_text);
  return result;
}

RobloxTextEditResult RobloxTextEditor::FinishLocked() {
  Status status = SyncLocked();
  if (status.ok()) {
    status = sink_.return_pressed(sink_.context, session_.textbox_handle);
  }
  bool ended = false;
  if (status.ok() && !session_.manual_focus_release) {
    status = PassLocked(true);
    if (status.ok()) {
      ClearLocked();
      ended = true;
    }
  }
  if (!status.ok()) {
    ++snapshot_.native_errors;
    sink_.release_focus(sink_.context, session_.textbox_handle);
    ClearLocked();
    ended = true;
  }
  const bool ok = status.ok();
  return Result(true, ok, ended, std::move(status));
}

RobloxTextEditResult RobloxTextEditor::ReleaseLocked() {
  Status status = sink_.release_focus(sink_.context, session_.textbox_handle);
  if (!status.ok()) {
    ++snapshot_.native_errors;
    return Result(true, false, false, std::move(status));
  }
  ClearLocked();
  return Result(true, true, true);
}

Status RobloxTextEditor::SyncLocked() {
  return sink_.sync(sink_.context, text_.data(), text_.size(),
                    snapshot_.cursor_utf16);
}

Status RobloxTextEditor::PassLocked(bool finished) {
  return sink_.pass_text(sink_.context, session_.textbox_handle, text_.data(),
                         text_.size(), finished, snapshot_.cursor_utf16);
}

void RobloxTextEditor::RememberPendingNativeEchoLocked() {
  if (!pending_native_echoes_.empty() &&
      pending_native_echoes_.back() == text_) {
    return;
  }
  while (!pending_native_echoes_.empty() &&
         (pending_native_echoes_.size() >= kMaximumPendingNativeEchoes ||
          pending_native_echo_bytes_ > kMaximumTextBytes - text_.size())) {
    pending_native_echo_bytes_ -= pending_native_echoes_.front().size();
    SecureClear(&pending_native_echoes_.front());
    pending_native_echoes_.pop_front();
  }
  pending_native_echoes_.push_back(text_);
  pending_native_echo_bytes_ += text_.size();
}

bool RobloxTextEditor::ConsumePendingNativeEchoLocked(const std::string& text) {
  const auto match = std::find(pending_native_echoes_.begin(),
                               pending_native_echoes_.end(), text);
  if (match == pending_native_echoes_.end()) {
    return false;
  }
  const auto settled = match + 1;
  for (auto state = pending_native_echoes_.begin(); state != settled; ++state) {
    pending_native_echo_bytes_ -= state->size();
    SecureClear(&*state);
  }
  pending_native_echoes_.erase(pending_native_echoes_.begin(), settled);
  return true;
}

void RobloxTextEditor::ClearPendingNativeEchoesLocked() {
  for (std::string& text : pending_native_echoes_) {
    SecureClear(&text);
  }
  pending_native_echoes_.clear();
  pending_native_echo_bytes_ = 0;
}

void RobloxTextEditor::PublishDisplayLocked(RobloxTextDisplayEvent event) {
  if (display_sink_.update == nullptr) {
    return;
  }
  RobloxTextDisplayUpdate update;
  update.event = event;
  update.generation = session_.generation;
  update.area_x = session_.area_x;
  update.area_y = session_.area_y;
  update.area_width = session_.area_width;
  update.area_height = session_.area_height;
  update.font_size = session_.font_size;
  update.multiline = session_.multiline;
  update.x_alignment = session_.x_alignment;
  update.y_alignment = session_.y_alignment;
  update.text_color = session_.text_color;
  update.font = session_.font;
  update.text_input_type = session_.text_input_type;
  update.text_wrapped = session_.text_wrapped;
  if (event != RobloxTextDisplayEvent::kHide) {
    update.utf8 = text_.data();
    update.utf8_size = text_.size();
    update.cursor_utf16 = snapshot_.cursor_utf16;
    update.selection_begin_utf16 = snapshot_.selection_begin_utf16;
    update.selection_end_utf16 = snapshot_.selection_end_utf16;
  }
  display_sink_.update(display_sink_.context, update);
}

void RobloxTextEditor::UpdateSelectionSnapshotLocked() {
  snapshot_.cursor_utf16 = Utf16Cursor(text_, cursor_byte_);
  snapshot_.selection_begin_utf16 =
      Utf16Cursor(text_, SelectionBeginLocked());
  snapshot_.selection_end_utf16 = Utf16Cursor(text_, SelectionEndLocked());
}

std::size_t RobloxTextEditor::SelectionBeginLocked() const {
  return std::min(cursor_byte_, selection_anchor_byte_);
}

std::size_t RobloxTextEditor::SelectionEndLocked() const {
  return std::max(cursor_byte_, selection_anchor_byte_);
}

bool RobloxTextEditor::HasSelectionLocked() const {
  return cursor_byte_ != selection_anchor_byte_;
}

void RobloxTextEditor::ClearLocked() {
  if (snapshot_.focused) {
    PublishDisplayLocked(RobloxTextDisplayEvent::kHide);
  }
  SecureClear(&session_.initial_text);
  session_ = {};
  SecureClear(&text_);
  ClearPendingNativeEchoesLocked();
  cursor_byte_ = 0;
  selection_anchor_byte_ = 0;
  composition_begin_byte_ = 0;
  composition_end_byte_ = 0;
  composition_original_cursor_byte_ = 0;
  composition_original_anchor_byte_ = 0;
  SecureClear(&composition_replaced_text_);
  composition_active_ = false;
  active_shortcuts_ = 0;
  pointer_drag_ = false;
  snapshot_.focused = false;
  snapshot_.textbox_handle = 0;
  snapshot_.generation = 0;
  snapshot_.text_bytes = 0;
  snapshot_.cursor_utf16 = 0;
  snapshot_.selection_begin_utf16 = 0;
  snapshot_.selection_end_utf16 = 0;
}

}  // namespace runtime
}  // namespace blox7
