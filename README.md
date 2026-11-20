# VeraCore

VeraCore là ứng dụng biên tập âm thanh và MIDI: giao diện Qt Widgets nằm trong `VeraCore.QT`, còn engine âm thanh nằm trong `VeraCore.Audio`. Solution hiện hỗ trợ Windows x64 (Debug và Release).

## Build trên Windows

Cài Visual Studio với workload Desktop development with C++, toolset v145, Windows SDK, Qt 6 cho MSVC x64 (dự án dùng Qt 6.3.1), Qt VS Tools và vcpkg có tích hợp MSBuild.

Từ Developer PowerShell, lấy đúng nhánh và các SDK đi kèm:

```powershell
git clone --branch codex/hybrid-arrangement-ui --recurse-submodules https://github.com/dodinhhoanganh/VeraCore.git
cd VeraCore
git -C 3rdparty/vst2sdk apply ../../patches/vst2sdk-host-linkage.patch
```

Patch chỉ cần áp dụng một lần sau khi clone. Nó sửa lỗi trùng định nghĩa khi nhiều tệp C++ dùng header VST2.

1. Đặt `QtRoot` trỏ tới bộ Qt MSVC x64, `QtMsBuild` tới thư mục chứa `qt.targets`, và `VCPKG_ROOT` tới vcpkg.
2. Build PortAudio bằng project trong `3rdparty/portaudio/build/msvc` ở cấu hình Release x64. Cả hai cấu hình VeraCore đều cần `portaudio_x64.lib` và `portaudio_x64.dll` trong thư mục `x64/Release` của project đó.
3. Chạy `msbuild VeraCore.sln /m /p:Configuration=Release /p:Platform=x64`. Manifest `vcpkg.json` khai báo các thư viện FFmpeg.
4. Chạy `x64/Release/VeraCore.QT.exe` với các DLL Qt và FFmpeg có trên PATH hoặc cạnh tệp EXE.

Xem [cấu hình backend âm thanh](docs/native-audio-backends.md) nếu cần dùng SDK tùy chọn. Các bài kiểm tra độc lập nằm trong `test/`.
