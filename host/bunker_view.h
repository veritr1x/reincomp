#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <functional>

namespace bunker::windows {
// The Bunker's pages (Pod Programs) shown over the game window in a WebView2
// pane. Ctrl+B opens and closes it. The launcher serves the pages and writes
// where they are to a state file: {"url","token"}, or {"error"}.
class BunkerView {
 public:
  // loader: WebView2Loader.dll. user_data: a folder the web view may write to.
  // Paths as the program's arguments give them.
  BunkerView(const std::string &loader,const std::string &state_file,const std::string &user_data);
  BunkerView(const std::wstring &loader,const std::wstring &state_file,const std::wstring &user_data);
  void OpenDashboard(void *parent,const std::wstring &path);
  using PageEvent = std::function<void(const std::wstring &,const std::wstring &)>;
  BunkerView(const std::string &loader,const std::string &user_data,void *parent,PageEvent notify);
  BunkerView(const std::wstring &loader,const std::wstring &user_data,void *parent,PageEvent notify);
  ~BunkerView();
  void PageFrame(int x,int y,int width,int height,int viewport_width,int viewport_height);
  void Navigate(const std::wstring &url);
  void Visible(bool visible);
  void Background(int red,int green,int blue,int alpha);
  void SetHeader(const std::wstring &name,const std::wstring &value);
  void ClearCache();
  void OpenExternalLinks(bool enabled);
  bool History(bool forward,bool move);
  void Reload();
  void Stop();
  bool IsVisible() const;
  static bool HasControllerArea();
  static uint32_t ControllerTarget(void *parent,long x,long y,uint32_t *dashboard);
  // The window's UI thread only, for each of its messages. True: consumed.
  bool Message(void *hwnd,uint32_t message,uint64_t wparam,int64_t lparam);
 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
}  // namespace bunker::windows
