#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "app.h"

namespace studio7 {
namespace {

const CLSID kWicFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
const GUID kPngContainer = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
const GUID kIcoContainer = {0xa3a860c4, 0x338f, 0x4c17, {0x91, 0x9a, 0xfb, 0xa4, 0xb5, 0x62, 0x8f, 0x21}};
const GUID kBgra = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};
const GUID kPremultipliedBgra = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};
const UINT kIconSizes[] = {16, 20, 24, 32, 40, 48, 64, 256};

template <class T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

std::wstring with_image(const std::wstring& file,
                        const std::function<std::wstring(IWICImagingFactory*, IWICBitmapDecoder*)>& use) {
  HMODULE ole = LoadLibraryW(L"ole32.dll");
  using InitFn = HRESULT(WINAPI*)(LPVOID, DWORD);
  using UninitFn = void(WINAPI*)();
  using CreateFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
  const auto init = ole ? reinterpret_cast<InitFn>(GetProcAddress(ole, "CoInitializeEx")) : nullptr;
  const auto uninit = ole ? reinterpret_cast<UninitFn>(GetProcAddress(ole, "CoUninitialize")) : nullptr;
  const auto create = ole ? reinterpret_cast<CreateFn>(GetProcAddress(ole, "CoCreateInstance")) : nullptr;
  if (!init || !uninit || !create) return L"Windows can't read images here.";
  const HRESULT com = init(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  IWICImagingFactory* wic = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  std::wstring error = L"Windows can't read this image.";
  if (SUCCEEDED(create(kWicFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&wic))) &&
      SUCCEEDED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
    error = use(wic, decoder);
  release(decoder);
  release(wic);
  if (SUCCEEDED(com)) uninit();
  return error;
}

bool square(IWICImagingFactory* wic, IWICBitmapSource* picture, UINT size, std::vector<uint8_t>* pixels) {
  UINT w = 0, h = 0;
  if (FAILED(picture->GetSize(&w, &h)) || !w || !h) return false;
  const double fit = static_cast<double>(size) / std::max(w, h);
  const UINT fw = std::clamp<UINT>(static_cast<UINT>(std::lround(w * fit)), 1, size);
  const UINT fh = std::clamp<UINT>(static_cast<UINT>(std::lround(h * fit)), 1, size);
  IWICFormatConverter* premultiplied = nullptr;
  IWICBitmapScaler* scaler = nullptr;
  IWICFormatConverter* straight = nullptr;
  std::vector<uint8_t> fitted(static_cast<size_t>(fw) * fh * 4);
  const bool ok = SUCCEEDED(wic->CreateFormatConverter(&premultiplied)) &&
                  SUCCEEDED(premultiplied->Initialize(picture, kPremultipliedBgra, WICBitmapDitherTypeNone, nullptr, 0,
                                                      WICBitmapPaletteTypeCustom)) &&
                  SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                  SUCCEEDED(scaler->Initialize(premultiplied, fw, fh, WICBitmapInterpolationModeFant)) &&
                  SUCCEEDED(wic->CreateFormatConverter(&straight)) &&
                  SUCCEEDED(straight->Initialize(scaler, kBgra, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
                  SUCCEEDED(straight->CopyPixels(nullptr, fw * 4, static_cast<UINT>(fitted.size()), fitted.data()));
  release(straight);
  release(scaler);
  release(premultiplied);
  if (!ok) return false;
  pixels->assign(static_cast<size_t>(size) * size * 4, 0);
  const UINT left = (size - fw) / 2, top = (size - fh) / 2;
  for (UINT y = 0; y < fh; ++y)
    std::memcpy(pixels->data() + (static_cast<size_t>(top + y) * size + left) * 4, fitted.data() + static_cast<size_t>(y) * fw * 4,
                static_cast<size_t>(fw) * 4);
  return true;
}

void put16(std::string* out, uint16_t value) { out->append(reinterpret_cast<const char*>(&value), 2); }
void put32(std::string* out, uint32_t value) { out->append(reinterpret_cast<const char*>(&value), 4); }

std::string icon_file(IWICImagingFactory* wic, IWICBitmapSource* picture) {
  const size_t count = std::size(kIconSizes);
  std::string directory, images;
  put16(&directory, 0);
  put16(&directory, 1);
  put16(&directory, static_cast<uint16_t>(count));
  for (const UINT size : kIconSizes) {
    std::vector<uint8_t> pixels;
    if (!square(wic, picture, size, &pixels)) return {};
    const uint32_t mask_row = ((size + 31) / 32) * 4;
    std::string image;
    put32(&image, 40);
    put32(&image, size);
    put32(&image, size * 2);
    put16(&image, 1);
    put16(&image, 32);
    for (int i = 0; i < 6; ++i) put32(&image, 0);
    for (UINT y = size; y-- > 0;) image.append(reinterpret_cast<const char*>(pixels.data()) + static_cast<size_t>(y) * size * 4, size * 4);
    image.append(static_cast<size_t>(mask_row) * size, '\0');
    directory.push_back(static_cast<char>(size == 256 ? 0 : size));
    directory.push_back(static_cast<char>(size == 256 ? 0 : size));
    put16(&directory, 0);
    put16(&directory, 1);
    put16(&directory, 32);
    put32(&directory, static_cast<uint32_t>(image.size()));
    put32(&directory, static_cast<uint32_t>(6 + count * 16 + images.size()));
    images += image;
  }
  return directory + images;
}

}  // namespace

std::wstring check_cursor(const std::wstring& file) {
  return with_image(file, [](IWICImagingFactory*, IWICBitmapDecoder* decoder) -> std::wstring {
    GUID format{};
    if (FAILED(decoder->GetContainerFormat(&format)) || format != kPngContainer) return L"Roblox only reads PNG cursors.";
    return {};
  });
}

std::wstring check_font(const std::wstring& file) {
  HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return L"Studio7 can't open this file.";
  char head[4] = {};
  DWORD got = 0;
  ReadFile(handle, head, sizeof head, &got, nullptr);
  CloseHandle(handle);
  const bool font = got == sizeof head && (memcmp(head, "\0\1\0\0", 4) == 0 || memcmp(head, "OTTO", 4) == 0 ||
                                           memcmp(head, "true", 4) == 0);
  return font ? std::wstring() : L"That isn't a .ttf or .otf font.";
}

std::wstring check_icon(const std::wstring& file) {
  return with_image(file, [](IWICImagingFactory*, IWICBitmapDecoder* decoder) -> std::wstring {
    UINT frames = 0;
    return SUCCEEDED(decoder->GetFrameCount(&frames)) && frames > 0 ? std::wstring() : L"Windows can't read this image.";
  });
}

std::wstring save_icon(const std::wstring& source, const std::wstring& target) {
  const std::wstring fresh = target + L".new";
  const std::wstring error = with_image(source, [&](IWICImagingFactory* wic, IWICBitmapDecoder* decoder) -> std::wstring {
    GUID format{};
    if (SUCCEEDED(decoder->GetContainerFormat(&format)) && format == kIcoContainer)
      return CopyFileW(source.c_str(), fresh.c_str(), FALSE) ? std::wstring() : L"Studio7 can't copy this icon.";
    UINT frames = 0, best = 0, best_area = 0;
    decoder->GetFrameCount(&frames);
    for (UINT i = 0; i < frames; ++i) {
      IWICBitmapFrameDecode* frame = nullptr;
      UINT w = 0, h = 0;
      if (SUCCEEDED(decoder->GetFrame(i, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w * h > best_area) {
        best = i;
        best_area = w * h;
      }
      release(frame);
    }
    IWICBitmapFrameDecode* frame = nullptr;
    if (!best_area || FAILED(decoder->GetFrame(best, &frame))) return L"Windows can't read this image.";
    const std::string bytes = icon_file(wic, frame);
    release(frame);
    if (bytes.empty()) return L"Windows can't read this image.";
    HANDLE out = CreateFileW(fresh.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return L"Studio7 can't save the icon.";
    DWORD written = 0;
    const bool ok = WriteFile(out, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(out);
    return ok ? std::wstring() : L"Studio7 can't save the icon.";
  });
  if (error.empty() && MoveFileExW(fresh.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) return {};
  DeleteFileW(fresh.c_str());
  return error.empty() ? L"Studio7 can't save the icon." : error;
}

}  // namespace studio7
