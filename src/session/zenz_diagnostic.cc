// Copyright 2026 Mozkey IbG contributors.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Mozkey IbG nor the names of its contributors
// may be used to endorse or promote products derived from this software
// without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "session/zenz_diagnostic.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mozc {
namespace session {
namespace {

uint32_t RotateRight(uint32_t value, unsigned amount) {
  return (value >> amount) | (value << (32 - amount));
}

class Sha256State final {
 public:
  Sha256State()
      : hash_({0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}) {}

  void Update(const uint8_t* data, size_t size) {
    total_bytes_ += size;
    while (size > 0) {
      const size_t available = block_.size() - block_size_;
      const size_t copied = std::min(available, size);
      std::copy(data, data + copied, block_.begin() + block_size_);
      block_size_ += copied;
      data += copied;
      size -= copied;
      if (block_size_ == block_.size()) {
        Transform(block_.data());
        block_size_ = 0;
      }
    }
  }

  std::string FinalHex() {
    const uint64_t total_bits = total_bytes_ * 8;
    block_[block_size_++] = 0x80;
    if (block_size_ > 56) {
      std::fill(block_.begin() + block_size_, block_.end(), uint8_t{0});
      Transform(block_.data());
      block_size_ = 0;
    }
    std::fill(block_.begin() + block_size_, block_.begin() + 56, uint8_t{0});
    block_size_ = 56;
    for (int shift = 56; shift >= 0; shift -= 8) {
      block_[block_size_++] = static_cast<uint8_t>(total_bits >> shift);
    }
    Transform(block_.data());

    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const uint32_t word : hash_) {
      for (int shift = 28; shift >= 0; shift -= 4) {
        result.push_back(kHex[(word >> shift) & 0x0f]);
      }
    }
    return result;
  }

 private:
  void Transform(const uint8_t* block) {
    static constexpr std::array<uint32_t, 64> kRoundConstants = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
        0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
        0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
        0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<uint32_t, 64> words = {};
    for (size_t i = 0; i < 16; ++i) {
      const size_t offset = i * 4;
      words[i] = (static_cast<uint32_t>(block[offset]) << 24) |
                 (static_cast<uint32_t>(block[offset + 1]) << 16) |
                 (static_cast<uint32_t>(block[offset + 2]) << 8) |
                 static_cast<uint32_t>(block[offset + 3]);
    }
    for (size_t i = 16; i < words.size(); ++i) {
      const uint32_t s0 = RotateRight(words[i - 15], 7) ^
                          RotateRight(words[i - 15], 18) ^
                          (words[i - 15] >> 3);
      const uint32_t s1 = RotateRight(words[i - 2], 17) ^
                          RotateRight(words[i - 2], 19) ^
                          (words[i - 2] >> 10);
      words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = hash_[0];
    uint32_t b = hash_[1];
    uint32_t c = hash_[2];
    uint32_t d = hash_[3];
    uint32_t e = hash_[4];
    uint32_t f = hash_[5];
    uint32_t g = hash_[6];
    uint32_t h = hash_[7];
    for (size_t i = 0; i < words.size(); ++i) {
      const uint32_t sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^
                            RotateRight(e, 25);
      const uint32_t choose = (e & f) ^ ((~e) & g);
      const uint32_t temp1 = h + sum1 + choose + kRoundConstants[i] + words[i];
      const uint32_t sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^
                            RotateRight(a, 22);
      const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t temp2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    hash_[0] += a;
    hash_[1] += b;
    hash_[2] += c;
    hash_[3] += d;
    hash_[4] += e;
    hash_[5] += f;
    hash_[6] += g;
    hash_[7] += h;
  }

  std::array<uint32_t, 8> hash_;
  std::array<uint8_t, 64> block_ = {};
  size_t block_size_ = 0;
  uint64_t total_bytes_ = 0;
};

std::string JsonEscape(absl::string_view value) {
  std::string output;
  output.reserve(value.size() + 16);

  constexpr char kHex[] = "0123456789abcdef";
  for (size_t position = 0; position < value.size();) {
    const unsigned char byte =
        static_cast<unsigned char>(value[position]);

    // Keep valid UTF-8 intact for readability.  If a model or transport
    // produces an invalid byte sequence, encode the individual byte as a
    // JSON escape instead; the corresponding Base64 field still retains the
    // exact original bytes and the JSONL line remains valid UTF-8.
    if (byte >= 0x80) {
      size_t length = 0;
      char32_t codepoint = 0;
      char32_t minimum = 0;
      if ((byte & 0xe0) == 0xc0) {
        length = 2;
        codepoint = byte & 0x1f;
        minimum = 0x80;
      } else if ((byte & 0xf0) == 0xe0) {
        length = 3;
        codepoint = byte & 0x0f;
        minimum = 0x800;
      } else if ((byte & 0xf8) == 0xf0) {
        length = 4;
        codepoint = byte & 0x07;
        minimum = 0x10000;
      }

      bool valid = length != 0 && position + length <= value.size();
      if (valid) {
        for (size_t i = 1; i < length; ++i) {
          const unsigned char continuation =
              static_cast<unsigned char>(value[position + i]);
          if ((continuation & 0xc0) != 0x80) {
            valid = false;
            break;
          }
          codepoint = (codepoint << 6) | (continuation & 0x3f);
        }
      }
      valid = valid && codepoint >= minimum && codepoint <= 0x10ffff &&
              !(codepoint >= 0xd800 && codepoint <= 0xdfff);
      if (valid) {
        output.append(value.data() + position, length);
        position += length;
        continue;
      }

      output += "\\u00";
      output.push_back(kHex[(byte >> 4) & 0x0f]);
      output.push_back(kHex[byte & 0x0f]);
      ++position;
      continue;
    }

    switch (byte) {
      case '\\':
        output += "\\\\";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (byte < 0x20) {
          output += "\\u00";
          output.push_back(kHex[(byte >> 4) & 0x0f]);
          output.push_back(kHex[byte & 0x0f]);
        } else {
          output.push_back(static_cast<char>(byte));
        }
        break;
    }
    ++position;
  }
  return output;
}

std::string Base64Encode(absl::string_view value) {
  constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string output;
  output.reserve((value.size() + 2) / 3 * 4);
  for (size_t i = 0; i < value.size(); i += 3) {
    const uint32_t first = static_cast<unsigned char>(value[i]);
    const uint32_t second =
        i + 1 < value.size() ? static_cast<unsigned char>(value[i + 1]) : 0;
    const uint32_t third =
        i + 2 < value.size() ? static_cast<unsigned char>(value[i + 2]) : 0;
    const uint32_t block = (first << 16) | (second << 8) | third;

    output.push_back(kAlphabet[(block >> 18) & 0x3f]);
    output.push_back(kAlphabet[(block >> 12) & 0x3f]);
    output.push_back(i + 1 < value.size() ? kAlphabet[(block >> 6) & 0x3f]
                                          : '=');
    output.push_back(i + 2 < value.size() ? kAlphabet[block & 0x3f] : '=');
  }
  return output;
}

bool IsContinuation(unsigned char byte) {
  return (byte & 0xc0) == 0x80;
}

// Decodes one code point.  Invalid UTF-8 is represented as U+FFFD and
// consumes one byte, while the original bytes remain available in the
// capture's Base64 field.
char32_t DecodeCodepoint(absl::string_view value, size_t* position) {
  const unsigned char first =
      static_cast<unsigned char>(value[*position]);
  if (first < 0x80) {
    ++*position;
    return first;
  }

  size_t length = 0;
  char32_t codepoint = 0;
  char32_t minimum = 0;
  if ((first & 0xe0) == 0xc0) {
    length = 2;
    codepoint = first & 0x1f;
    minimum = 0x80;
  } else if ((first & 0xf0) == 0xe0) {
    length = 3;
    codepoint = first & 0x0f;
    minimum = 0x800;
  } else if ((first & 0xf8) == 0xf0) {
    length = 4;
    codepoint = first & 0x07;
    minimum = 0x10000;
  } else {
    ++*position;
    return 0xfffd;
  }

  if (*position + length > value.size()) {
    ++*position;
    return 0xfffd;
  }

  for (size_t i = 1; i < length; ++i) {
    const unsigned char byte =
        static_cast<unsigned char>(value[*position + i]);
    if (!IsContinuation(byte)) {
      ++*position;
      return 0xfffd;
    }
    codepoint = (codepoint << 6) | (byte & 0x3f);
  }

  if (codepoint < minimum || codepoint > 0x10ffff ||
      (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
    ++*position;
    return 0xfffd;
  }

  *position += length;
  return codepoint;
}

std::string CodepointName(char32_t codepoint) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "U+%04X",
                static_cast<unsigned int>(codepoint));
  return buffer;
}

#if defined(_WIN32)

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return std::string();
  }
  const int input_size = static_cast<int>(value.size());
  const int output_size = WideCharToMultiByte(
      CP_UTF8, 0, value.data(), input_size, nullptr, 0, nullptr, nullptr);
  if (output_size <= 0) {
    return std::string();
  }
  std::string output(output_size, '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, value.data(), input_size, output.data(),
                          output_size, nullptr, nullptr) <= 0) {
    return std::string();
  }
  return output;
}

std::string UserDiagnosticPath() {
  constexpr wchar_t kSubkey[] = L"Environment";
  constexpr wchar_t kName[] = L"MOZC_ZENZ_DIAGNOSTIC_JSONL";
  DWORD size = 0;
  LSTATUS status = RegGetValueW(
      HKEY_CURRENT_USER, kSubkey, kName,
      RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, nullptr,
      &size);
  if (status != ERROR_SUCCESS || size < sizeof(wchar_t)) {
    return std::string();
  }

  std::wstring value(size / sizeof(wchar_t), L'\0');
  status = RegGetValueW(
      HKEY_CURRENT_USER, kSubkey, kName,
      RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr,
      value.data(), &size);
  if (status != ERROR_SUCCESS) {
    return std::string();
  }
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return WideToUtf8(value);
}

#endif

std::string DiagnosticPath() {
#if defined(_WIN32)
  // Prefer the persistent value on Windows.  A long-lived parent process can
  // retain a stale inherited value even after the user changes the setting.
  const std::string user_path = UserDiagnosticPath();
  if (!user_path.empty()) {
    return user_path;
  }
#endif
  const char* value = std::getenv("MOZC_ZENZ_DIAGNOSTIC_JSONL");
  if (value != nullptr && *value != '\0') {
    return std::string(value);
  }
  return std::string();
}

std::mutex& DiagnosticMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

std::atomic<uint64_t>& DiagnosticSequence() {
  static auto* sequence = new std::atomic<uint64_t>(0);
  return *sequence;
}

uint64_t UnixMicros() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

#if defined(_WIN32)

std::wstring Utf8ToWide(absl::string_view value) {
  if (value.empty()) {
    return std::wstring();
  }
  const int input_size = static_cast<int>(value.size());
  const int output_size = MultiByteToWideChar(
      CP_UTF8, 0, value.data(), input_size, nullptr, 0);
  if (output_size <= 0) {
    return std::wstring();
  }
  std::wstring output(output_size, L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, value.data(), input_size, output.data(),
                          output_size) <= 0) {
    return std::wstring();
  }
  return output;
}

bool AppendLine(absl::string_view path, absl::string_view line) {
  const std::wstring wide_path = Utf8ToWide(path);
  if (wide_path.empty()) {
    return false;
  }

  HANDLE file = CreateFileW(
      wide_path.c_str(), FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }

  size_t offset = 0;
  bool ok = true;
  while (offset < line.size()) {
    const DWORD remaining = static_cast<DWORD>(
        std::min<size_t>(line.size() - offset, 1u << 20));
    DWORD written = 0;
    if (!WriteFile(file, line.data() + offset, remaining, &written, nullptr) ||
        written == 0) {
      ok = false;
      break;
    }
    offset += written;
  }
  CloseHandle(file);
  return ok;
}

#else

bool AppendLine(absl::string_view path, absl::string_view line) {
  int flags = O_WRONLY | O_CREAT | O_APPEND;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const int fd = ::open(std::string(path).c_str(), flags, 0600);
  if (fd < 0) {
    return false;
  }

  // Captures contain user text and model output.  Keep an existing capture
  // private as well; this is still explicitly opt-in by the caller.
  ::fchmod(fd, 0600);

  size_t offset = 0;
  bool ok = true;
  while (offset < line.size()) {
    const ssize_t written =
        ::write(fd, line.data() + offset, line.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      ok = false;
      break;
    }
    offset += static_cast<size_t>(written);
  }
  ::close(fd);
  return ok;
}

#endif

std::string ComputeSha256File(absl::string_view path) {
  Sha256State state;
  std::array<uint8_t, 64 * 1024> buffer = {};
#if defined(_WIN32)
  const std::wstring wide_path = Utf8ToWide(path);
  if (wide_path.empty()) {
    return std::string();
  }
  HANDLE file = CreateFileW(
      wide_path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::string();
  }
  while (true) {
    DWORD read_size = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                  &read_size, nullptr)) {
      CloseHandle(file);
      return std::string();
    }
    if (read_size == 0) {
      break;
    }
    state.Update(buffer.data(), read_size);
  }
  CloseHandle(file);
#else
  std::ifstream file(std::string(path.data(), path.size()),
                     std::ios::in | std::ios::binary);
  if (!file) {
    return std::string();
  }
  while (file) {
    file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    const std::streamsize read_size = file.gcount();
    if (read_size > 0) {
      state.Update(buffer.data(), static_cast<size_t>(read_size));
    }
  }
  if (file.bad()) {
    return std::string();
  }
#endif
  return state.FinalHex();
}

std::mutex& Sha256Mutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

std::unordered_map<std::string, std::string>& Sha256Cache() {
  static auto* cache = new std::unordered_map<std::string, std::string>();
  return *cache;
}

}  // namespace

void ZenzDiagnosticJson::AddRawValue(absl::string_view name,
                                      std::string value) {
  if (!fields_.empty()) {
    fields_.push_back(',');
  }
  fields_.push_back('"');
  fields_ += JsonEscape(name);
  fields_ += "\":";
  fields_ += std::move(value);
}

void ZenzDiagnosticJson::AddString(absl::string_view name,
                                    absl::string_view value) {
  AddRawValue(name, std::string("\"") + JsonEscape(value) + "\"");
}

void ZenzDiagnosticJson::AddUint64(absl::string_view name, uint64_t value) {
  AddRawValue(name, std::to_string(value));
}

void ZenzDiagnosticJson::AddBool(absl::string_view name, bool value) {
  AddRawValue(name, value ? "true" : "false");
}

void ZenzDiagnosticJson::AddObject(absl::string_view name,
                                   absl::string_view object) {
  AddRawValue(name, std::string(object));
}

void ZenzDiagnosticJson::AddBase64(absl::string_view name,
                                    absl::string_view bytes) {
  AddString(name, Base64Encode(bytes));
}

void ZenzDiagnosticJson::AddCodepoints(absl::string_view name,
                                        absl::string_view utf8) {
  std::string array = "[";
  size_t position = 0;
  while (position < utf8.size()) {
    if (array.size() > 1) {
      array.push_back(',');
    }
    array.push_back('"');
    array += CodepointName(DecodeCodepoint(utf8, &position));
    array.push_back('"');
  }
  array.push_back(']');
  AddRawValue(name, std::move(array));
}

std::string ZenzDiagnosticJson::Build() const {
  return "{" + fields_ + "}";
}

bool ZenzDiagnosticCapture::IsEnabled() {
  return !DiagnosticPath().empty();
}

std::string ZenzDiagnosticCapture::Sha256File(absl::string_view path) {
  if (path.empty()) {
    return std::string();
  }

  const std::string key(path.data(), path.size());
  {
    std::lock_guard<std::mutex> lock(Sha256Mutex());
    const auto found = Sha256Cache().find(key);
    if (found != Sha256Cache().end()) {
      return found->second;
    }
  }

  const std::string digest = ComputeSha256File(path);
  std::lock_guard<std::mutex> lock(Sha256Mutex());
  Sha256Cache().emplace(key, digest);
  return digest;
}

void ZenzDiagnosticCapture::Write(const ZenzDiagnosticJson& event) {
  const std::string path = DiagnosticPath();
  if (path.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(DiagnosticMutex());
  std::string line = event.Build();
  if (line.size() < 2 || line.front() != '{' || line.back() != '}') {
    return;
  }

  const uint64_t sequence = DiagnosticSequence().fetch_add(1) + 1;
  const std::string metadata =
      "\"schema_version\":1,\"sequence\":" +
      std::to_string(sequence) + ",\"timestamp_unix_micros\":" +
      std::to_string(UnixMicros());
  line.insert(1, metadata + (line.size() > 2 ? "," : ""));
  line.push_back('\n');
  AppendLine(path, line);
}

}  // namespace session
}  // namespace mozc
