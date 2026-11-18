#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace bunker::windows {
// The Bunker's pages (Pod Programs) shown over the game window in a WebView2
// pane. Ctrl+B opens and closes it. The launcher serves the pages and writes
// where they are to a state file: {"url","token"}, or {"error"}.
class BunkerView {
 public:
  // loader: WebView2Loader.dll. user_data: a folder the web view may write to.
  // Paths as the program's arguments give them.
  BunkerView(const std::string &loader,const std::string &state_file,const std::string &user_data);
  ~BunkerView();
  // The window's UI thread only, for each of its messages. True: consumed.
  bool Message(void *hwnd,uint32_t message,uint64_t wparam,int64_t lparam);
 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
}  // namespace bunker::windows
