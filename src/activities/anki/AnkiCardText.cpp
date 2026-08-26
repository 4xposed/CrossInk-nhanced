#include "AnkiCardText.h"

#include <cstring>

namespace {
constexpr size_t kBlockSeparatorBytes = 2;

bool isFieldInsideBuffer(const CardField& field, const char* buffer, const size_t bufferCapacity) {
  if (field.text == nullptr || field.length == 0 || field.length > kMaxCardFieldTextBytes ||
      bufferCapacity < static_cast<size_t>(field.length) + 1) {
    return false;
  }

  const uintptr_t bufferStart = reinterpret_cast<uintptr_t>(buffer);
  const uintptr_t bufferEnd = bufferStart + bufferCapacity;
  const uintptr_t fieldStart = reinterpret_cast<uintptr_t>(field.text);
  return fieldStart >= bufferStart && fieldStart <= bufferEnd - (static_cast<size_t>(field.length) + 1);
}
}  // namespace

bool flattenCardFieldsInPlace(const std::array<CardField, kMaxCardFields>& fields, const uint8_t fieldCount,
                              char* const buffer, const size_t bufferCapacity, FlattenedCardText& out) {
  out = {};
  if (buffer == nullptr || fieldCount == 0 || fieldCount > fields.size()) return false;

  size_t flattenedLength = 0;
  for (uint8_t index = 0; index < fieldCount; ++index) {
    const CardField& field = fields[index];
    if (!isFieldInsideBuffer(field, buffer, bufferCapacity) ||
        flattenedLength > kMaxCardSideTextBytes - field.length) {
      return false;
    }
    flattenedLength += field.length;
    if (index != 0) {
      if (flattenedLength > kMaxCardSideTextBytes - kBlockSeparatorBytes) return false;
      flattenedLength += kBlockSeparatorBytes;
    }
  }
  if (flattenedLength + 1 > bufferCapacity) return false;

  uint16_t fieldStart = 0;
  out.fieldCount = fieldCount;
  for (uint8_t index = 0; index < fieldCount; ++index) {
    const CardField& field = fields[index];
    out.fieldStarts[index] = fieldStart;
    out.fieldLengths[index] = field.length;
    out.fieldPrimary[index] = field.primary;
    fieldStart = static_cast<uint16_t>(fieldStart + field.length + (index + 1 < fieldCount ? kBlockSeparatorBytes : 0));
  }

  char* write = buffer + bufferCapacity - 1;
  *write = '\0';
  for (uint8_t index = fieldCount; index > 0; --index) {
    const CardField& field = fields[index - 1];
    write -= field.length;
    std::memmove(write, field.text, field.length);
    if (index > 1) {
      write -= kBlockSeparatorBytes;
      write[0] = '\n';
      write[1] = '\n';
    }
  }

  out.text = write;
  out.length = static_cast<uint16_t>(flattenedLength);
  return true;
}
