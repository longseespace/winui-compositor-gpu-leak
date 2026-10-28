// IslandLeakRepro: a bare Win32 window hosting WinUI 3 (Windows App SDK) through
// DesktopWindowXamlSource, imitating a chat app switching chats: every step the
// sidebar's selection moves and the content area is replaced with a new page
// (a header, then a scroll view of message bubbles and multi-paragraph replies
// with an action row), scrolled to its end. Measure the process's GPU memory
// ("GPU Process Memory" counters) while it runs.
//
//   IslandLeakRepro.exe [steps=300] [intervalMs=1200] [--no-mica]
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>

#include <string>

#pragma comment(lib, "dwmapi.lib")

using namespace winrt;
namespace xaml = Microsoft::UI::Xaml;
namespace controls = Microsoft::UI::Xaml::Controls;
namespace documents = Microsoft::UI::Xaml::Documents;
namespace media = Microsoft::UI::Xaml::Media;
using Color = Windows::UI::Color;

// An Application for the XAML on this thread, with WinUI's control resources.
struct App : xaml::ApplicationT<App, xaml::Markup::IXamlMetadataProvider> {
  xaml::Markup::IXamlType GetXamlType(Windows::UI::Xaml::Interop::TypeName const &type) { return m_provider.GetXamlType(type); }
  xaml::Markup::IXamlType GetXamlType(hstring const &fullName) { return m_provider.GetXamlType(fullName); }
  com_array<xaml::Markup::XmlnsDefinition> GetXmlnsDefinitions() { return m_provider.GetXmlnsDefinitions(); }

 private:
  xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider m_provider;
};

namespace {

xaml::Hosting::DesktopWindowXamlSource g_source{nullptr};
controls::StackPanel g_sidebar{nullptr};
controls::Border g_page{nullptr};
int g_step = 0;
int g_steps = 300;

const wchar_t *kWords[] = {L"compositor", L"surface", L"layer", L"window", L"message", L"reply", L"island", L"memory",
                           L"switch", L"render", L"bubble", L"scroll", L"paragraph", L"theme", L"backdrop", L"atlas"};

std::wstring Sentence(int seed, int words) {
  std::wstring text;
  for (int i = 0; i < words; ++i) {
    if (i) text += L' ';
    text += kWords[(seed * 7 + i * 13) % 16];
  }
  text += L'.';
  return text;
}

media::SolidColorBrush Brush(uint8_t a, uint8_t r, uint8_t g, uint8_t b) { return media::SolidColorBrush{Color{a, r, g, b}}; }

xaml::UIElement UserBubble(int seed) {
  controls::TextBlock words;
  words.Text(Sentence(seed, 6 + seed % 14));
  words.TextWrapping(xaml::TextWrapping::Wrap);
  words.FontSize(14);
  controls::Border bubble;
  bubble.Background(Brush(0x18, 255, 255, 255));
  bubble.CornerRadius({16, 16, 16, 16});
  bubble.Padding({14, 9, 14, 9});
  bubble.HorizontalAlignment(xaml::HorizontalAlignment::Right);
  bubble.MaxWidth(520);
  bubble.Child(words);
  return bubble;
}

xaml::UIElement Reply(int seed) {
  controls::StackPanel reply;
  reply.Spacing(8);
  controls::RichTextBlock prose;
  prose.FontSize(14);
  prose.LineHeight(22);
  prose.TextWrapping(xaml::TextWrapping::Wrap);
  prose.IsTextSelectionEnabled(true);
  const int paragraphs = 1 + seed % 5;
  for (int p = 0; p < paragraphs; ++p) {
    documents::Paragraph paragraph;
    paragraph.Margin({0, 0, 0, 10});
    documents::Run bold;
    bold.Text(Sentence(seed + p, 3) + L' ');
    bold.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    paragraph.Inlines().Append(bold);
    documents::Run run;
    run.Text(Sentence(seed * 3 + p, 18 + (seed + p) % 40));
    paragraph.Inlines().Append(run);
    prose.Blocks().Append(paragraph);
  }
  reply.Children().Append(prose);
  // The action row: copy, like, dislike, regenerate, more.
  controls::StackPanel actions;
  actions.Orientation(controls::Orientation::Horizontal);
  actions.Spacing(14);
  for (wchar_t glyph : {L'\xE8C8', L'\xE8E1', L'\xE8E0', L'\xE72C', L'\xE712'}) {
    controls::FontIcon icon;
    icon.Glyph(hstring(&glyph, 1));
    icon.FontSize(14);
    icon.Opacity(0.7);
    actions.Children().Append(icon);
  }
  reply.Children().Append(actions);
  return reply;
}

// A chat page: header, then the messages in a scroll view, scrolled to the end.
xaml::UIElement Page(int step) {
  controls::Grid page;
  controls::RowDefinition header;
  header.Height(xaml::GridLengthHelper::Auto());
  controls::RowDefinition body;
  body.Height(xaml::GridLengthHelper::FromValueAndType(1, xaml::GridUnitType::Star));
  page.RowDefinitions().Append(header);
  page.RowDefinitions().Append(body);

  controls::StackPanel title;
  title.Padding({16, 10, 16, 10});
  controls::TextBlock name;
  name.Text(L"Chat " + std::to_wstring(step) + L": " + Sentence(step, 4));
  name.FontSize(14);
  name.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
  controls::TextBlock subtitle;
  subtitle.Text(L"Default Agent");
  subtitle.FontSize(12);
  subtitle.Opacity(0.6);
  title.Children().Append(name);
  title.Children().Append(subtitle);
  page.Children().Append(title);

  controls::ScrollViewer scroll;
  controls::Grid::SetRow(scroll, 1);
  controls::StackPanel list;
  list.Spacing(16);
  list.MaxWidth(836);
  list.Padding({16, 8, 16, 96});
  controls::TextBlock date;
  date.Text(L"October " + std::to_wstring(1 + step % 28) + L", 2026");
  date.FontSize(12);
  date.Opacity(0.5);
  date.HorizontalAlignment(xaml::HorizontalAlignment::Center);
  list.Children().Append(date);
  for (int i = 0; i < 14; ++i) {
    const int seed = step * 31 + i;
    list.Children().Append(i % 2 == 0 ? UserBubble(seed) : Reply(seed));
  }
  scroll.Content(list);
  page.Children().Append(scroll);
  // Weak: a handler holding its own scroll view keeps the whole page alive.
  scroll.Loaded([weak = make_weak(scroll)](auto &&, auto &&) {
    if (auto view = weak.get()) {
      view.UpdateLayout();
      view.ChangeView(nullptr, Windows::Foundation::IReference<double>{view.ScrollableHeight()}, nullptr, true);
    }
  });
  return page;
}

void Switch() {
  ++g_step;
  // The sidebar's selection moves to the next row.
  auto rows = g_sidebar.Children();
  for (uint32_t i = 0; i < rows.Size(); ++i) {
    rows.GetAt(i).as<controls::Border>().Background(i == static_cast<uint32_t>(g_step % rows.Size()) ? Brush(0x14, 255, 255, 255) : nullptr);
  }
  g_page.Child(Page(g_step));
}

xaml::UIElement Root() {
  controls::Grid root;
  root.RequestedTheme(xaml::ElementTheme::Dark);
  controls::ColumnDefinition side;
  side.Width(xaml::GridLengthHelper::FromValueAndType(260, xaml::GridUnitType::Pixel));
  controls::ColumnDefinition main;
  main.Width(xaml::GridLengthHelper::FromValueAndType(1, xaml::GridUnitType::Star));
  root.ColumnDefinitions().Append(side);
  root.ColumnDefinitions().Append(main);

  g_sidebar = controls::StackPanel{};
  g_sidebar.Padding({8, 48, 8, 8});
  for (int i = 0; i < 18; ++i) {
    controls::TextBlock label;
    label.Text(Sentence(i * 5, 3 + i % 4));
    label.FontSize(14);
    label.TextTrimming(xaml::TextTrimming::CharacterEllipsis);
    controls::Border row;
    row.CornerRadius({4, 4, 4, 4});
    row.Padding({12, 8, 12, 8});
    row.Child(label);
    g_sidebar.Children().Append(row);
  }
  root.Children().Append(g_sidebar);

  controls::Grid content;
  controls::Grid::SetColumn(content, 1);
  content.Background(Brush(0x0C, 255, 255, 255));
  content.CornerRadius({8, 0, 0, 0});
  content.Margin({0, 40, 0, 0});
  g_page = controls::Border{};
  content.Children().Append(g_page);
  // The composer stays, as in the app.
  controls::Border composer;
  composer.VerticalAlignment(xaml::VerticalAlignment::Bottom);
  composer.Margin({16, 0, 16, 16});
  composer.MaxWidth(820);
  composer.Height(72);
  composer.CornerRadius({8, 8, 8, 8});
  composer.Background(Brush(0x10, 255, 255, 255));
  composer.BorderBrush(Brush(0x14, 255, 255, 255));
  composer.BorderThickness({1, 1, 1, 1});
  controls::TextBlock placeholder;
  placeholder.Text(L"Ask anything");
  placeholder.Margin({12, 10, 12, 10});
  placeholder.Opacity(0.6);
  composer.Child(placeholder);
  content.Children().Append(composer);
  root.Children().Append(content);
  return root;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_SIZE:
      if (g_source) {
        g_source.SiteBridge().MoveAndResize({0, 0, LOWORD(lparam), HIWORD(lparam)});
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  int interval = 1200;
  bool mica = true;
  for (int i = 1, n = 0; i < argc; ++i) {
    std::wstring arg = argv[i];
    if (arg == L"--no-mica") mica = false;
    else if (n++ == 0) g_steps = _wtoi(argv[i]);
    else interval = _wtoi(argv[i]);
  }

  init_apartment(apartment_type::single_threaded);
  auto controller = Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnCurrentThread();
  auto app = make<App>();
  auto manager = xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
  app.Resources().MergedDictionaries().Append(controls::XamlControlsResources{});

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"IslandLeakRepro";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Island leak repro", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860,
                              nullptr, nullptr, instance, nullptr);
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

  g_source = xaml::Hosting::DesktopWindowXamlSource{};
  g_source.Initialize(Microsoft::UI::GetWindowIdFromWindow(hwnd));
  auto root = Root().as<controls::Grid>();
  // Without Mica, the window's dark base (Mica's own fallback color).
  if (!mica) root.Background(Brush(255, 0x20, 0x20, 0x20));
  g_source.Content(root);
  RECT client{};
  GetClientRect(hwnd, &client);
  g_source.SiteBridge().MoveAndResize({0, 0, client.right, client.bottom});
  g_source.SiteBridge().Show();
  ShowWindow(hwnd, SW_SHOW);
  // Set before the source was shown, the backdrop crashed WinUI.
  if (mica) g_source.SystemBackdrop(media::MicaBackdrop{});
  Switch();

  auto timer = controller.DispatcherQueue().CreateTimer();
  timer.Interval(std::chrono::milliseconds(interval));
  timer.Tick([timer](auto &&, auto &&) {
    if (g_step >= g_steps) {
      timer.Stop();
      return;
    }
    Switch();
  });
  timer.Start();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  timer.Stop();
  g_page = nullptr;
  g_sidebar = nullptr;
  g_source.Close();
  g_source = nullptr;
  manager.Close();
  controller.ShutdownQueue();
  return 0;
}
