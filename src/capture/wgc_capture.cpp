#include "capture/wgc_capture.h"

#include <d3d11.h>
#include <dxgi.h>
#include <roapi.h>
#include <windows.foundation.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winstring.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <future>

#include <gst/app/gstappsrc.h>
#include <gst/d3d11/gstd3d11.h>
#include <gst/video/video.h>

namespace streamline {

using Microsoft::WRL::ComPtr;
namespace wgc = ABI::Windows::Graphics::Capture;
namespace wgd = ABI::Windows::Graphics::DirectX;
namespace wgd3d = ABI::Windows::Graphics::DirectX::Direct3D11;

namespace {

constexpr auto kPixelFormat = wgd::DirectXPixelFormat_B8G8R8A8UIntNormalized;
constexpr int kPoolBuffers = 2;

template <size_t N>
ComPtr<IInspectable> activation_factory(const wchar_t (&class_name)[N], REFIID iid, HRESULT& hr) {
  HSTRING_HEADER header;
  HSTRING name = nullptr;
  ComPtr<IInspectable> factory;
  hr = WindowsCreateStringReference(class_name, N - 1, &header, &name);
  if (SUCCEEDED(hr)) hr = RoGetActivationFactory(name, iid, reinterpret_cast<void**>(factory.GetAddressOf()));
  return factory;
}

void post_error(GstElement* src, const char* text) {
  GError* err = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ, text);
  gst_element_post_message(src, gst_message_new_error(GST_OBJECT(src), err, nullptr));
  g_error_free(err);
}

void close(IInspectable* obj) {
  ComPtr<ABI::Windows::Foundation::IClosable> closable;
  if (obj && SUCCEEDED(obj->QueryInterface(IID_PPV_ARGS(&closable)))) closable->Close();
}

}  // namespace

struct WgcCapture::State {
  HWND hwnd = nullptr;
  GstD3D11Device* device = nullptr;  // ref held
  GstElement* appsrc = nullptr;      // ref held
  int fps = 60;
  ComPtr<wgd3d::IDirect3DDevice> winrt_device;
  ComPtr<wgc::IGraphicsCaptureItem> item;
  ComPtr<wgc::IDirect3D11CaptureFramePool> pool;
  ComPtr<wgc::IGraphicsCaptureSession> session;
  ABI::Windows::Graphics::SizeInt32 pool_size{};
  // Output side: caps/buffer pool follow the window's content size.
  int width = 0;
  int height = 0;
  GstBufferPool* buffers = nullptr;
  GstBuffer* last = nullptr;

  ~State() {
    gst_clear_buffer(&last);
    if (buffers) {
      gst_buffer_pool_set_active(buffers, FALSE);
      gst_object_unref(buffers);
    }
    close(session.Get());
    close(pool.Get());
    if (appsrc) gst_object_unref(appsrc);
    if (device) gst_object_unref(device);
  }
};

bool WgcCapture::open(HWND hwnd, GstD3D11Device* device, int& width, int& height,
                      std::string& error) {
  stop();
  auto* s = new State;
  s->hwnd = hwnd;
  s->device = static_cast<GstD3D11Device*>(gst_object_ref(device));

  // WinRT objects are created on the capture thread (MTA); wait for the item size.
  std::promise<std::string> ready;
  auto result = ready.get_future();
  running_ = true;
  thread_ = std::thread([this, s, &ready] {
    RoInitialize(RO_INIT_MULTITHREADED);
    std::string err;
    HRESULT hr = S_OK;

    ID3D11Device* d3d = gst_d3d11_device_get_device_handle(s->device);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IInspectable> inspectable;
    if (FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
        FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.GetAddressOf())) ||
        FAILED(inspectable.As(&s->winrt_device))) {
      err = "Falha ao preparar o Direct3D para captura.";
    }

    if (err.empty()) {
      auto interop = activation_factory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem,
                                        __uuidof(IGraphicsCaptureItemInterop), hr);
      ComPtr<IGraphicsCaptureItemInterop> item_interop;
      if (FAILED(hr) || FAILED(interop.As(&item_interop)) ||
          FAILED(item_interop->CreateForWindow(s->hwnd, __uuidof(wgc::IGraphicsCaptureItem),
                                               reinterpret_cast<void**>(s->item.GetAddressOf())))) {
        err = "Esta janela não pode ser capturada (Windows.Graphics.Capture recusou).";
      }
    }

    if (err.empty()) {
      auto statics = activation_factory(
          RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool,
          __uuidof(wgc::IDirect3D11CaptureFramePoolStatics2), hr);
      ComPtr<wgc::IDirect3D11CaptureFramePoolStatics2> pool_statics;
      s->item->get_Size(&s->pool_size);
      if (FAILED(hr) || FAILED(statics.As(&pool_statics)) ||
          FAILED(pool_statics->CreateFreeThreaded(s->winrt_device.Get(), kPixelFormat, kPoolBuffers,
                                                  s->pool_size, s->pool.GetAddressOf())) ||
          FAILED(s->pool->CreateCaptureSession(s->item.Get(), s->session.GetAddressOf()))) {
        err = "Falha ao iniciar a captura da janela.";
      }
    }

    if (err.empty()) {
      ComPtr<wgc::IGraphicsCaptureSession2> session2;
      if (SUCCEEDED(s->session.As(&session2))) session2->put_IsCursorCaptureEnabled(true);
      // Windows 11 only; on Windows 10 the yellow border can't be turned off.
      ComPtr<wgc::IGraphicsCaptureSession3> session3;
      if (SUCCEEDED(s->session.As(&session3))) session3->put_IsBorderRequired(false);
      s->width = s->pool_size.Width;
      s->height = s->pool_size.Height;
    }

    ready.set_value(err);
    if (!err.empty()) {
      delete s;
      RoUninitialize();
      return;
    }
    run(s);
    RoUninitialize();
  });

  error = result.get();
  if (!error.empty()) {
    thread_.join();
    running_ = false;
    return false;
  }
  state_ = s;
  width = s->width & ~1;
  height = s->height & ~1;
  return true;
}

void WgcCapture::start(GstElement* appsrc, int fps) {
  if (!state_) return;
  state_->appsrc = static_cast<GstElement*>(gst_object_ref(appsrc));
  state_->fps = fps;
  started_.store(true);
}

void WgcCapture::stop() {
  if (!thread_.joinable()) return;
  running_ = false;
  thread_.join();
  state_ = nullptr;  // deleted by the thread
  started_ = false;
}

namespace {

// (Re)configure appsrc caps and a D3D11 buffer pool for a new output size.
bool configure_output(GstElement* appsrc, GstD3D11Device* device, int w, int h, int fps,
                      GstBufferPool*& pool) {
  if (pool) {
    gst_buffer_pool_set_active(pool, FALSE);
    gst_object_unref(pool);
    pool = nullptr;
  }
  GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA", "width",
                                      G_TYPE_INT, w, "height", G_TYPE_INT, h, "framerate",
                                      GST_TYPE_FRACTION, fps, 1, nullptr);
  gst_caps_set_features(caps, 0, gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY, nullptr));
  GstVideoInfo info;
  gst_video_info_from_caps(&info, caps);

  pool = gst_d3d11_buffer_pool_new(device);
  GstStructure* config = gst_buffer_pool_get_config(pool);
  gst_buffer_pool_config_set_params(config, caps, GST_VIDEO_INFO_SIZE(&info), 0, 0);
  GstD3D11AllocationParams* params = gst_d3d11_allocation_params_new(
      device, &info, GST_D3D11_ALLOCATION_FLAG_DEFAULT,
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0);
  gst_buffer_pool_config_set_d3d11_allocation_params(config, params);
  gst_d3d11_allocation_params_free(params);
  bool ok = gst_buffer_pool_set_config(pool, config) && gst_buffer_pool_set_active(pool, TRUE);

  gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
  gst_caps_unref(caps);
  return ok;
}

}  // namespace

void WgcCapture::run(State* s) {
  using clock = std::chrono::steady_clock;
  // Wait for start() (or stop() before starting).
  while (running_ && !started_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (!running_ || FAILED(s->session->StartCapture())) {
    if (running_) post_error(s->appsrc, "Falha ao iniciar a captura da janela.");
    delete s;
    return;
  }

  auto period = std::chrono::nanoseconds(1'000'000'000LL / std::max(1, s->fps));
  auto next = clock::now();
  bool output_ready = false;

  while (running_) {
    next += period;
    std::this_thread::sleep_until(next);
    if (clock::now() - next > period * 4) next = clock::now();  // don't try to catch up

    if (!IsWindow(s->hwnd)) {
      post_error(s->appsrc, "A janela capturada foi fechada.");
      break;
    }

    // Take the newest frame, dropping older ones.
    ComPtr<wgc::IDirect3D11CaptureFrame> frame;
    for (;;) {
      ComPtr<wgc::IDirect3D11CaptureFrame> f;
      if (FAILED(s->pool->TryGetNextFrame(f.GetAddressOf())) || !f) break;
      if (frame) close(frame.Get());
      frame = f;
    }

    if (frame) {
      ABI::Windows::Graphics::SizeInt32 content{};
      frame->get_ContentSize(&content);
      int w = std::max(2, content.Width) & ~1;
      int h = std::max(2, content.Height) & ~1;

      // Window resized: recreate the WGC pool so later frames match the content size.
      if (content.Width != s->pool_size.Width || content.Height != s->pool_size.Height) {
        s->pool_size = content;
        s->pool->Recreate(s->winrt_device.Get(), kPixelFormat, kPoolBuffers, content);
      }
      if (!output_ready || w != s->width || h != s->height) {
        s->width = w;
        s->height = h;
        output_ready = configure_output(s->appsrc, s->device, w, h, s->fps, s->buffers);
        if (!output_ready) {
          post_error(s->appsrc, "Falha ao alocar texturas D3D11 para a captura.");
          break;
        }
      }

      ComPtr<wgd3d::IDirect3DSurface> surface;
      ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
      ComPtr<ID3D11Texture2D> src;
      GstBuffer* buf = nullptr;
      if (SUCCEEDED(frame->get_Surface(surface.GetAddressOf())) && SUCCEEDED(surface.As(&access)) &&
          SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&src))) &&
          gst_buffer_pool_acquire_buffer(s->buffers, &buf, nullptr) == GST_FLOW_OK) {
        GstMemory* mem = gst_buffer_peek_memory(buf, 0);
        GstMapInfo map;
        if (gst_memory_map(mem, &map, static_cast<GstMapFlags>(GST_MAP_WRITE | GST_MAP_D3D11))) {
          auto* dst = reinterpret_cast<ID3D11Resource*>(map.data);  // GST_MAP_D3D11
          UINT index = gst_d3d11_memory_get_subresource_index(GST_D3D11_MEMORY_CAST(mem));
          D3D11_BOX box{0, 0, 0, static_cast<UINT>(std::min(w, content.Width)),
                        static_cast<UINT>(std::min(h, content.Height)), 1};
          gst_d3d11_device_lock(s->device);
          gst_d3d11_device_get_device_context_handle(s->device)
              ->CopySubresourceRegion(dst, index, 0, 0, 0, src.Get(), 0, &box);
          gst_d3d11_device_unlock(s->device);
          gst_memory_unmap(mem, &map);
          gst_clear_buffer(&s->last);
          s->last = buf;
        } else {
          gst_buffer_unref(buf);
        }
      }
      close(frame.Get());
    }

    // New frame or a repeat of the last one, stamped with the pipeline running time.
    // (appsrc's do-timestamp only sets DTS; encoders/payloaders need PTS.)
    if (s->last) {
      GstClock* clock = gst_element_get_clock(s->appsrc);
      if (clock) {
        GstClockTime now = gst_clock_get_time(clock) - gst_element_get_base_time(s->appsrc);
        gst_object_unref(clock);
        GstBuffer* out = gst_buffer_copy(s->last);
        GST_BUFFER_PTS(out) = now;
        GST_BUFFER_DTS(out) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DURATION(out) = GST_SECOND / s->fps;
        gst_app_src_push_buffer(GST_APP_SRC(s->appsrc), out);
      }
    }
  }
  delete s;
}

}  // namespace streamline
