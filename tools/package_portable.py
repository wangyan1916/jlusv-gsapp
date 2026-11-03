"""Build a self-contained Windows x64 distribution from the existing Release."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

ROOT=Path(__file__).resolve().parents[1]

def package(qt):
    version=re.search(r'project\(jlusv_gsapp VERSION ([0-9.]+)',(ROOT/'CMakeLists.txt').read_text()).group(1)
    name='JLUSV-GSApp-'+version+'-Windows-x64'
    release=ROOT/'build/Release'
    stage=ROOT/'build/portable-stage'/name
    archive=ROOT/'build/portable-dist'/(name+'-portable.zip')
    assert (release/'jlusv_gsapp.exe').is_file(),'Build Release first'
    assert not stage.exists() and not archive.exists(),'Preserve existing package; choose a new version or inspect it'
    stage.mkdir(parents=True);archive.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(release/'jlusv_gsapp.exe',stage/'jlusv_gsapp.exe')
    env=dict(os.environ)
    env['PATH']=str(qt/'bin')+os.pathsep+env.get('PATH',env.get('Path',''))
    result=subprocess.run([str(qt/'bin/windeployqt.exe'),'--release','--qmldir',str(ROOT/'qml'),
        '--dir',str(stage),'--compiler-runtime','--translations','zh_CN','--no-virtualkeyboard',
        str(stage/'jlusv_gsapp.exe')],env=env,capture_output=True,text=True,encoding='utf8',errors='replace')
    (ROOT/'build/portable-stage/windeployqt.log').write_text(result.stdout+result.stderr,encoding='utf8')
    assert result.returncode==0,result.stdout+result.stderr
    # The existing CMake Release already carries the exact MSVC runtime used.
    for pattern in ('msvcp140*.dll','vcruntime140*.dll','concrt140.dll','vccorlib140.dll'):
        for runtime in release.glob(pattern):shutil.copy2(runtime,stage/runtime.name)
    (stage/'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=.\nQml2Imports=.\nTranslations=translations\n',encoding='utf8')
    tiles=ROOT/'maps/tiles';assert tiles.is_dir()
    shutil.copytree(tiles,stage/'maps/tiles')
    tile_files=[p for p in tiles.rglob('*') if p.is_file()]
    instructions=f"""JLUSV GSApp {version} Windows x64 便携版

1. 将整个ZIP解压到一个可读写的目录，双击 jlusv_gsapp.exe。
2. 请复制整个文件夹，保留全部DLL、插件、QML目录、qt.conf和maps；不要只复制EXE。
3. 已包含Qt和MSVC运行库，无需安装Qt、Visual Studio或Python。适用于64位Windows；地图需要正常的显卡OpenGL驱动。
4. 已附当前离线卫星地图（{len(tile_files)}个文件）。覆盖范围与原电脑相同；无网络也可使用这些地图。其他地区可添加maps/tiles/{{z}}/{{x}}/{{y}}.png。
5. 连接地面站前，填写实际网关地址和控制密码；包中不保存密码、设备会话或个人配置。
6. 地图本地服务使用127.0.0.1:18080；同一电脑一次运行一个实例，端口被占用时先关闭旧实例。
7. 船位、GS位置、相对距离与东/北坐标需要网关提供新鲜有效数据；回转/Z型需配套EXP1固件和IPC0.4.5。
8. 正式运动前确认目标船、控制模式和反馈；本次打包测试使用模拟网关，未向设备发送运动指令。

程序版本未改变控制逻辑。保留原有地图坐标系选项，任务坐标始终使用WGS-84。
"""
    (stage/'使用说明.txt').write_text(instructions,encoding='utf-8-sig')
    shutil.copy2(ROOT/'README.md',stage/'README.md')
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for path in sorted(stage.rglob('*')):
            if path.is_file():z.write(path,name+'/'+path.relative_to(stage).as_posix())
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        assert z.read(name+'/jlusv_gsapp.exe')==(release/'jlusv_gsapp.exe').read_bytes()
    info=dict(version=version,source_commit=subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip(),
        archive=str(archive),archive_bytes=archive.stat().st_size,sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
        stage=str(stage),exe_sha256=hashlib.sha256((stage/'jlusv_gsapp.exe').read_bytes()).hexdigest(),
        tile_files=len(tile_files),tile_bytes=sum(p.stat().st_size for p in tile_files),
        archive_valid=True,hardware_actions=0)
    (archive.parent/(name+'-package-info.json')).write_text(json.dumps(info,indent=2)+'\n',encoding='utf8')
    print(json.dumps(info,indent=2))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-dir',type=Path)
    args=parser.parse_args()
    if args.qt_dir is None:
        cache=(ROOT/'build/CMakeCache.txt').read_text()
        args.qt_dir=Path(re.search(r'^Qt5_DIR:PATH=(.+)$',cache,re.M).group(1)).parents[2]
    package(args.qt_dir)
