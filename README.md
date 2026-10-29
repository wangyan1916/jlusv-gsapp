# JLUSV 地面站上位机 (Qt 5.15.3 C++17)

本子项目是专为无人船 (USV) 打造的桌面端地面站程序，使用支持 C++17 的 Qt 5.15.3 开发。

## 主要特性

1. **高精度遥测展示**：
   - 接收地面站 ESP32 网关（`GET /api/v1/fleet/overview`）广播的船只实时 RTK 定位、对地航速、真北艏向、电量与卫星解状态。
2. **高分卫星底图与打点循迹**：
   - 基于 `QtLocation` 与 `QtQuickWidgets`，支持加载吉林一号/高德高分辨率卫星底图；
   - 支持本地离线瓦片缓存（MBTiles 或 XYZ 瓦片目录），并已在 `.gitignore` 中过滤大体积地图文件；
   - 鼠标直接在地图上打点规划循迹折线轨迹。
3. **坐标纠偏引擎 (WGS-84 <-> GCJ-02)**：
   - 内置高精度双向坐标转换算法，消除国内卫星切片非线性偏移，确保地图打点下发到船端 RTK 航行时轨迹分毫不差。
4. **任务下发与应急控制**：
   - 按照 ESP32 现有的 `/api/v1/missions/submit` 格式，支持单次下发 1~16 个目标航点及期望巡航速度；
   - 提供一键紧急制动停机功能。

## 构建与运行

### 依赖环境
- **编译器**：MSVC 2022 (msvc143_x64) 支持 C++17
- **Qt 版本**：Qt 5.15.3 64-bit (路径通常为 `E:\Qt\5.15.3\msvc143_x64`)
- **构建工具**：CMake >= 3.16 与 Ninja / NMake

### CMake 构建命令示例 (PowerShell)
```powershell
$env:Qt5_DIR = "E:\Qt\5.15.3\msvc143_x64\lib\cmake\Qt5"
cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```
