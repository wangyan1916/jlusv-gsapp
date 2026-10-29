import QtQuick 2.15
import QtQuick.Controls 2.15
import QtLocation 5.15
import QtPositioning 5.15

Item {
    id: root
    visible: true
    clip: true  // 防止放大后溢出地图窗口

    // 虚拟缩放级别：16.0 ~ 22.0
    // 16.0 ~ 19.0: 物理高分卫星切片渲染
    // 19.0 ~ 22.0: GPU 硬件级平滑超分辨率放大 (解决 QtLocation 超过 19 级黑屏问题，图片无级放大)
    property real currentZoom: 18.2
    readonly property real minZoom: 16.0
    readonly property real maxZoom: 22.0

    // 晏湖水心精准中心坐标 (吉林大学前卫南区晏湖)
    readonly property var yanhuCenter: QtPositioning.coordinate(43.82540, 125.28870)

    function applyZoom(newZoom) {
        currentZoom = Math.max(minZoom, Math.min(maxZoom, newZoom));
        if (currentZoom <= 19.0) {
            map.zoomLevel = currentZoom;
            map.scale = 1.0;
        } else {
            map.zoomLevel = 19.0;
            // 按照墨卡托 2 的指数级平滑放大图像
            map.scale = Math.pow(2.0, currentZoom - 19.0);
        }
    }

    Plugin {
        id: mapPlugin
        name: "osm"

        // 本地离线高分卫星瓦片服务器 (http://127.0.0.1:18080)
        PluginParameter {
            name: "osm.mapping.custom.host"
            value: "http://127.0.0.1:18080/"
        }
        PluginParameter {
            name: "osm.useragent"
            value: "JLUSV GroundStation"
        }
        PluginParameter {
            name: "osm.mapping.providersrepository.disabled"
            value: true
        }
    }

    Map {
        id: map
        anchors.fill: parent
        plugin: mapPlugin
        transformOrigin: Item.Center

        // 精准定位于晏湖湖心
        center: root.yanhuCenter
        zoomLevel: 18.2
        minimumZoomLevel: 16.0
        maximumZoomLevel: 19.0  // 限制在 QtLocation OSM 插件稳定上限，防止黑屏

        onSupportedMapTypesChanged: {
            for (var i = 0; i < supportedMapTypes.length; ++i) {
                if (supportedMapTypes[i].style === MapType.CustomMap) {
                    activeMapType = supportedMapTypes[i];
                    return;
                }
            }
            if (supportedMapTypes.length > 0) {
                activeMapType = supportedMapTypes[supportedMapTypes.length - 1];
            }
        }

        // 循迹折线轨迹
        MapPolyline {
            id: trackLine
            line.width: 4
            line.color: "#00E5FF"
            path: mapBridge.waypointsQml
        }

        // 航点标记
        MapItemView {
            model: mapBridge.waypointsQml
            delegate: MapQuickItem {
                coordinate: modelData
                anchorPoint.x: 14
                anchorPoint.y: 14
                sourceItem: Rectangle {
                    width: 28
                    height: 28
                    radius: 14
                    color: "#E53935"
                    border.color: "white"
                    border.width: 2.5
                    Text {
                        anchors.centerIn: parent
                        text: (index + 1).toString()
                        color: "white"
                        font.bold: true
                        font.pixelSize: 12
                    }
                }
            }
        }

        // 船只实时位置与航向图标
        MapQuickItem {
            id: boatMarker
            coordinate: QtPositioning.coordinate(mapBridge.boatLat, mapBridge.boatLon)
            anchorPoint.x: 18
            anchorPoint.y: 18
            visible: mapBridge.boatLat !== 0.0 || mapBridge.boatLon !== 0.0

            sourceItem: Item {
                width: 36
                height: 36
                rotation: mapBridge.boatHeading

                Canvas {
                    anchors.fill: parent
                    onPaint: {
                        var ctx = getContext("2d");
                        ctx.reset();
                        ctx.fillStyle = "#FFEA00";
                        ctx.strokeStyle = "#D50000";
                        ctx.lineWidth = 2.5;
                        ctx.beginPath();
                        ctx.moveTo(18, 2);
                        ctx.lineTo(32, 32);
                        ctx.lineTo(18, 25);
                        ctx.lineTo(4, 32);
                        ctx.closePath();
                        ctx.fill();
                        ctx.stroke();
                    }
                }
            }
        }

        // 鼠标交互：滚轮无级缩放 + 左键打点 + 拖拽平移
        MouseArea {
            id: mouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton

            property real lastX: 0
            property real lastY: 0
            property bool isDragging: false
            property real pressX: 0
            property real pressY: 0

            onPressed: {
                pressX = mouse.x;
                pressY = mouse.y;
                lastX = mouse.x;
                lastY = mouse.y;
                isDragging = false;
            }

            onPositionChanged: {
                if (mouse.buttons & (Qt.LeftButton | Qt.RightButton)) {
                    var dx = mouse.x - lastX;
                    var dy = mouse.y - lastY;
                    var totalDist = Math.abs(mouse.x - pressX) + Math.abs(mouse.y - pressY);
                    if (totalDist > 5) {
                        isDragging = true;
                        // 按比例修正平移量
                        map.pan(-dx / map.scale, -dy / map.scale);
                    }
                    lastX = mouse.x;
                    lastY = mouse.y;
                }
            }

            onReleased: {
                if (!isDragging && mouse.button === Qt.LeftButton) {
                    var coord = map.toCoordinate(Qt.point(mouse.x, mouse.y));
                    if (coord.isValid) {
                        mapBridge.addWaypointFromMap(coord.latitude, coord.longitude, 1.2);
                    }
                }
                isDragging = false;
            }

            // 鼠标滚轮缩放
            onWheel: {
                var step = (wheel.angleDelta.y > 0) ? 0.35 : -0.35;
                root.applyZoom(root.currentZoom + step);
                wheel.accepted = true;
            }
        }
    }

    // 悬浮缩放控制按钮组 (+ / −)
    Column {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 20
        spacing: 8

        Rectangle {
            width: 42
            height: 42
            radius: 8
            color: "#FFFFFF"
            border.color: "#BDBDBD"
            border.width: 1
            opacity: 0.92

            Text {
                anchors.centerIn: parent
                text: "+"
                font.pixelSize: 26
                font.bold: true
                color: "#212121"
            }
            MouseArea {
                anchors.fill: parent
                onClicked: root.applyZoom(root.currentZoom + 0.5)
            }
        }

        Rectangle {
            width: 42
            height: 42
            radius: 8
            color: "#FFFFFF"
            border.color: "#BDBDBD"
            border.width: 1
            opacity: 0.92

            Text {
                anchors.centerIn: parent
                text: "−"
                font.pixelSize: 26
                font.bold: true
                color: "#212121"
            }
            MouseArea {
                anchors.fill: parent
                onClicked: root.applyZoom(root.currentZoom - 0.5)
            }
        }
    }

    // 快捷居中按钮：重置湖心 / 锁定船只
    Rectangle {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 20
        width: 110
        height: 38
        radius: 8
        color: "#1976D2"
        opacity: 0.92

        Text {
            anchors.centerIn: parent
            text: (mapBridge.boatLat !== 0.0 && mapBridge.boatLon !== 0.0) ? "锁定船只" : "居中晏湖"
            color: "white"
            font.bold: true
            font.pixelSize: 13
        }

        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (mapBridge.boatLat !== 0.0 && mapBridge.boatLon !== 0.0) {
                    map.center = QtPositioning.coordinate(mapBridge.boatLat, mapBridge.boatLon);
                } else {
                    map.center = root.yanhuCenter;
                    root.applyZoom(18.5);
                }
            }
        }
    }
}
