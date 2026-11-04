import QtQuick 2.15
import QtQuick.Controls 2.15
import QtLocation 5.15
import QtPositioning 5.15

Item {
    id: root
    visible: true
    clip: true  // 确保放大后不溢出地图窗口

    // 缩放级别：15.0 ~ 22.0
    // 15.0 ~ 19.0: 物理高清晰度卫星切片
    // 19.0 ~ 22.0: 硬件级无级平滑放大图片 (保证绝不黑屏、绝不出现“无卫星图”提示，图片可深度放大)
    property real currentZoom: 18.5
    property bool pickingGroundStation: false
    readonly property real minZoom: 15.0
    readonly property real maxZoom: 22.0

    // 晏湖中心精准坐标 (吉林大学前卫南区晏湖水心及湖心岛精确像素锚定)
    readonly property var yanhuCenter: QtPositioning.coordinate(43.82544, 125.28832)

    function applyZoom(newZoom) {
        currentZoom = Math.max(minZoom, Math.min(maxZoom, newZoom));
        if (currentZoom <= 19.0) {
            map.zoomLevel = currentZoom;
            map.scale = 1.0;
        } else {
            map.zoomLevel = 19.0;
            // 超过 19 级后直接平滑无级放大卫星图片本身，彻底杜绝无图空白，支持放大 1~8 倍
            map.scale = Math.pow(2.0, currentZoom - 19.0);
        }
    }

    Component.onCompleted: {
        root.applyZoom(18.5);
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
        // 彻底禁用 QtLocation 磁盘缓存，防止缓存历史错误切片或“无卫星图”占位图
        PluginParameter {
            name: "osm.mapping.cache.disk.size"
            value: 0
        }
    }

    Map {
        id: map
        objectName: "navigationMap"
        anchors.fill: parent
        plugin: mapPlugin
        transformOrigin: Item.Center

        // 默认视野绝对居中于晏湖水心
        center: root.yanhuCenter
        zoomLevel: 18.5
        minimumZoomLevel: 15.0
        maximumZoomLevel: 19.0

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

        MapQuickItem {
            id: groundStationMarker
            objectName: "groundStationMarker"
            coordinate: QtPositioning.coordinate(mapBridge.groundStationLat, mapBridge.groundStationLon)
            anchorPoint.x: 18
            anchorPoint.y: 18
            visible: mapBridge.groundStationValid
            sourceItem: Column {
                spacing: 3
                Rectangle {
                    width: 36
                    height: 36
                    radius: 6
                    color: "#1565C0"
                    border.color: "white"
                    border.width: 2
                    Text {
                        anchors.centerIn: parent
                        text: "GS"
                        color: "white"
                        font.bold: true
                        font.pixelSize: 14
                    }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "地面站"
                    color: "white"
                    style: Text.Outline
                    styleColor: "#263238"
                    font.bold: true
                    font.pixelSize: 13
                }
            }
        }

        // 船只实时位置与航向图标
        MapQuickItem {
            id: boatMarker
            objectName: "boatMarker"
            coordinate: QtPositioning.coordinate(mapBridge.boatLat, mapBridge.boatLon)
            anchorPoint.x: 18
            anchorPoint.y: 18
            visible: mapBridge.isOnline && (mapBridge.boatLat !== 0.0 || mapBridge.boatLon !== 0.0)

            sourceItem: Item {
                width: 36
                height: 36

                Canvas {
                    objectName: "boatSymbol"
                    // MapQuickItem sets its source item's origin to TopLeft.
                    // Rotate this child so the position anchor stays at the centre.
                    transformOrigin: Item.Center
                    rotation: mapBridge.boatHeadingValid ? mapBridge.boatHeading : 0
                    property bool directionValid: mapBridge.boatHeadingValid
                    onDirectionValidChanged: requestPaint()
                    anchors.fill: parent
                    onPaint: {
                        var ctx = getContext("2d");
                        ctx.reset();
                        ctx.fillStyle = "#FFEA00";
                        ctx.strokeStyle = "#D50000";
                        ctx.lineWidth = 2.5;
                        ctx.beginPath();
                        if (directionValid) {
                            ctx.moveTo(18, 2);
                            ctx.lineTo(32, 32);
                            ctx.lineTo(18, 25);
                            ctx.lineTo(4, 32);
                        } else {
                            ctx.arc(18, 18, 10, 0, 2 * Math.PI);
                        }
                        ctx.closePath();
                        ctx.fill();
                        ctx.stroke();
                    }
                }
            }
        }

        // 鼠标交互：滚轮无级缩放 + 左键点选航点 + 拖拽平移
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
                        map.pan(-dx, -dy);
                    }
                    lastX = mouse.x;
                    lastY = mouse.y;
                }
            }

            onReleased: {
                if (!isDragging && mouse.button === Qt.LeftButton) {
                    var coord = map.toCoordinate(Qt.point(mouse.x, mouse.y));
                    if (coord.isValid) {
                        if (root.pickingGroundStation) {
                            if (mapBridge.setGroundStationFromMap(coord.latitude, coord.longitude))
                                root.pickingGroundStation = false;
                        } else {
                            mapBridge.addWaypointFromMap(coord.latitude, coord.longitude, mapBridge.defaultSpeed);
                        }
                    }
                }
                isDragging = false;
            }

            // 鼠标滚轮缩放：平滑无级放大缩小
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
            width: 44
            height: 44
            radius: 8
            color: "#FFFFFF"
            border.color: "#BDBDBD"
            border.width: 1
            opacity: 0.95

            Text {
                anchors.centerIn: parent
                text: "+"
                font.pixelSize: 28
                font.bold: true
                color: "#212121"
            }
            MouseArea {
                anchors.fill: parent
                onClicked: root.applyZoom(root.currentZoom + 0.5)
            }
        }

        Rectangle {
            width: 44
            height: 44
            radius: 8
            color: "#FFFFFF"
            border.color: "#BDBDBD"
            border.width: 1
            opacity: 0.95

            Text {
                anchors.centerIn: parent
                text: "−"
                font.pixelSize: 28
                font.bold: true
                color: "#212121"
            }
            MouseArea {
                anchors.fill: parent
                onClicked: root.applyZoom(root.currentZoom - 0.5)
            }
        }
    }

    Rectangle {
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 14
        width: Math.min(parent.width - 100, 440)
        height: 48
        radius: 6
        color: "#E65100"
        visible: root.pickingGroundStation
        Text {
            anchors.fill: parent
            anchors.margins: 6
            text: "点击地图设置 GS 实际位置；船位与航点同步校正\n原始相对米数保持。再次点击“取消手点”可退出。"
            color: "white"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }
    }

    // 悬浮居中控制按钮组
    Column {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 20
        spacing: 10

        Button {
            width: 110
            height: 38
            text: root.pickingGroundStation ? "取消手点" : "地图手点 GS"
            enabled: mapBridge.groundReferenceValid || root.pickingGroundStation
            onClicked: root.pickingGroundStation = !root.pickingGroundStation
        }
        Button {
            width: 110
            height: 38
            text: "GS 恢复自动"
            enabled: mapBridge.manualGroundStation
            onClicked: {
                root.pickingGroundStation = false;
                mapBridge.useAutomaticGroundStation();
            }
        }

        Rectangle {
            width: 110
            height: 38
            radius: 8
            color: "#1565C0"
            border.color: "#FFFFFF"
            border.width: 1.5
            opacity: 0.95
            visible: mapBridge.groundStationValid

            Text {
                anchors.centerIn: parent
                text: "定位地面站"
                color: "white"
                font.bold: true
                font.pixelSize: 13
            }
            MouseArea {
                anchors.fill: parent
                onClicked: map.center = QtPositioning.coordinate(mapBridge.groundStationLat, mapBridge.groundStationLon)
            }
        }

        // 1. 永久“居中晏湖”按钮（任何时候点击均绝对重置视野至晏湖湖心）
        Rectangle {
            width: 110
            height: 38
            radius: 8
            color: "#1976D2"
            border.color: "#FFFFFF"
            border.width: 1.5
            opacity: 0.95

            Text {
                anchors.centerIn: parent
                text: "居中晏湖"
                color: "white"
                font.bold: true
                font.pixelSize: 13
            }

            MouseArea {
                anchors.fill: parent
                onClicked: {
                    map.center = root.yanhuCenter;
                    root.applyZoom(18.5);
                }
            }
        }

        // 2. “定位船只”按钮（仅当接收到有效经纬度且不在 0,0 时显示）
        Rectangle {
            width: 110
            height: 38
            radius: 8
            color: "#388E3C"
            border.color: "#FFFFFF"
            border.width: 1.5
            opacity: 0.95
            visible: mapBridge.boatLat !== 0.0 || mapBridge.boatLon !== 0.0

            Text {
                anchors.centerIn: parent
                text: "定位船只"
                color: "white"
                font.bold: true
                font.pixelSize: 13
            }

            MouseArea {
                anchors.fill: parent
                onClicked: {
                    if (mapBridge.boatLat !== 0.0 && mapBridge.boatLon !== 0.0) {
                        map.center = QtPositioning.coordinate(mapBridge.boatLat, mapBridge.boatLon);
                    }
                }
            }
        }
    }
}
