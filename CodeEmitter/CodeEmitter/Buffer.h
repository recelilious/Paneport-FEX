// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace ARMEmitter {
class Buffer {
public:
  Buffer() {
    SetBuffer(nullptr, 0);
  }

  Buffer(uint8_t* Base, uint64_t BaseSize) {
    SetBuffer(Base, BaseSize);
  }

  void SetBuffer(uint8_t* Base, uint64_t BaseSize) {
    BufferBase = Base;
    CurrentOffset = BufferBase;
    Size = BaseSize;
    WriteDelta = 0;
  }

  // Sets the distance from the buffer's addresses to the addresses through which its memory is
  // written, for a buffer of generated code on a host that maps such memory twice: executable at
  // the buffer's addresses and writable at others. The cursor, the labels and every address that
  // the emitter hands out stay addresses of the buffer; only the stores go through the distance.
  // SetBuffer resets it to 0.
  void SetWriteDelta(ptrdiff_t Delta) {
    WriteDelta = Delta;
  }

  // The address through which the memory at an address of the buffer is written.
  template<typename T>
  T* WriteAddress(T* Address) const {
    return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(Address) + WriteDelta);
  }

  template<typename T>
  requires (std::is_trivially_copyable_v<T>)
  void dcn(const T& Data) {
    std::memcpy(CurrentOffset + WriteDelta, &Data, sizeof(Data));
    CurrentOffset += sizeof(Data);
  }
  void dc8(uint8_t Data) {
    dcn(Data);
  }
  void dc16(uint16_t Data) {
    dcn(Data);
  }
  void dc32(uint32_t Data) {
    dcn(Data);
  }
  void dc64(uint64_t Data) {
    dcn(Data);
  }

  void EmitString(const char* String) {
    const auto StringLength = strlen(String);
    memcpy(CurrentOffset + WriteDelta, String, StringLength);
    CurrentOffset += StringLength;
  }

  void Align(size_t Size = 4) {
    // Align the buffer to provided size.
    auto CurrentAlignment = reinterpret_cast<uint64_t>(CurrentOffset) & (Size - 1);
    if (!CurrentAlignment) {
      return;
    }
    std::memset(CurrentOffset + WriteDelta, 0, Size - CurrentAlignment);
    CurrentOffset += Size - CurrentAlignment;
  }

  template<typename T>
  T GetCursorAddress() const {
    return reinterpret_cast<T>(CurrentOffset);
  }

  static void ClearICache(void* Begin, std::size_t Length) {
    __builtin___clear_cache(static_cast<char*>(Begin), static_cast<char*>(Begin) + Length);
  }

  size_t GetCursorOffset() const {
    return static_cast<size_t>(CurrentOffset - BufferBase);
  }

  uint8_t* GetBufferBase() const {
    return BufferBase;
  }

  void CursorIncrement(size_t Size) {
    CurrentOffset += Size;
  }

  void SetCursorOffset(size_t Offset) {
    CurrentOffset = BufferBase + Offset;
  }

  uint64_t GetBufferSize() const {
    return Size;
  }

  template<typename T>
  size_t GetCursorOffsetFromAddress(const T* Address) const {
    return static_cast<size_t>(reinterpret_cast<const uint8_t*>(Address) - BufferBase);
  }

protected:

  uint8_t* BufferBase;
  uint8_t* CurrentOffset;
  uint64_t Size;
  ptrdiff_t WriteDelta {};
};
} // namespace ARMEmitter
