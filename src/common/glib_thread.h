#pragma once

#include <functional>
#include <thread>

#include <glib.h>

namespace streamline {

// A thread running a GMainLoop on its own GMainContext (pushed as thread-default,
// so libsoup async calls made from it dispatch back to it).
class GlibThread {
 public:
  ~GlibThread() { stop(); }

  void start() {
    if (loop_) return;
    context_ = g_main_context_new();
    loop_ = g_main_loop_new(context_, FALSE);
    thread_ = std::thread([this] {
      g_main_context_push_thread_default(context_);
      g_main_loop_run(loop_);
      g_main_context_pop_thread_default(context_);
    });
  }

  // Runs `fn` on the thread (immediately if already on it).
  void invoke(std::function<void()> fn) {
    if (!context_) return;
    g_main_context_invoke_full(
        context_, G_PRIORITY_DEFAULT,
        [](gpointer p) -> gboolean {
          (*static_cast<std::function<void()>*>(p))();
          return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(fn)),
        [](gpointer p) { delete static_cast<std::function<void()>*>(p); });
  }

  // Runs `cleanup` on the thread, then stops and joins it.
  void stop(std::function<void()> cleanup = {}) {
    if (!loop_) return;
    invoke([this, cleanup = std::move(cleanup)] {
      if (cleanup) cleanup();
      g_main_loop_quit(loop_);
    });
    thread_.join();
    g_main_loop_unref(loop_);
    g_main_context_unref(context_);
    loop_ = nullptr;
    context_ = nullptr;
  }

 private:
  GMainContext* context_ = nullptr;
  GMainLoop* loop_ = nullptr;
  std::thread thread_;
};

}  // namespace streamline
