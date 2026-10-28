# WinUI 3 compositor GPU memory growth repro

A bare Win32 window hosting WinUI 3 (Windows App SDK 1.8.260508005) through
`DesktopWindowXamlSource`. Every 1.2 s it moves a sidebar selection and
replaces the content area with a new page (a header, then a `ScrollViewer` of
rounded translucent `Border` bubbles and multi-paragraph `RichTextBlock`
replies with a `FontIcon` action row), scrolled to its end, as a chat app does
when switching chats.

The process's GPU memory grows with every page and is never handed back, while
its handles and private bytes stay flat (the replaced pages are released).

## Build and run

Visual Studio 2026 (v145 toolset), Windows SDK 10.0.26100; packages restore
from NuGet:

    MSBuild IslandLeakRepro.vcxproj /restore /p:Configuration=Release /p:Platform=x64
    x64\Release\IslandLeakRepro.exe [steps=300] [intervalMs=1200] [--no-mica]

Needs the Windows App Runtime 1.8 installed (unpackaged app, bootstrapper).
Watch the `\GPU Process Memory(pid_<pid>*)\Shared Usage` and
`Dedicated Usage` counters (Performance Monitor, or `Get-Counter`).

## Results

Dell Latitude 5420, Intel Iris Xe (all GPU memory is shared), Windows 11,
counters sampled every 5 s over about 286 page switches:

| Run | GPU shared | Handles | Private bytes |
| --- | --- | --- | --- |
| Mica | 53 → 135 MB (+0.29 MB / switch), 138 MB a minute after the last switch | 823 → 823 | 72 → 79 MB |
| `--no-mica` | 64 → 153 MB (+0.31 MB / switch), 154 MB after | 819 → 807 | 76 → 78 MB |

## Where the memory is

A WPR trace (GPU profile) of the same pattern in a larger app hosting WinUI
the same way, with every GPU allocation paired with its free, put all memory
still live after 40 switches in the in-process compositor (`dwmcorei.dll`,
`CLocalAppRenderTarget::RenderDirtyRect`), none in app code:

| Live | Allocated by |
| --- | --- |
| 22.5 MB | `d2d1!CBaseRenderTarget::PushLayer` ← `D2DLayer::CreateBitmaps` ← `dwmcorei!CD2DContext::EndDraw` ← `CLocalAppRenderTarget::RenderDirtyRect` (Direct2D layer bitmaps) |
| 20.2 MB | `dcomp!CAtlasSurfacePool::Initialize` ← `CSurfaceManager::RequestLargeSurface` ← `dwmcorei!CDCompAtlasSurface::BeginDraw` ← `CLocalAppRenderTarget::RenderDirtyRect` |
| 15.5 MB | `dcomp!CAtlasSurfacePool::Initialize` ← `CDCompAtlasSurface::Create` ← `CLocalAppRenderTarget::EnsureRenderSurface` ← `CheckOcclusionState` |

Windows App SDK 2.6.4 showed the same growth in that app.
