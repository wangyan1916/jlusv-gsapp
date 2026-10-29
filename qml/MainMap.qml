import QtQuick 2.15
import QtQuick.Controls 2.15
import QtLocation 5.15
import QtPositioning 5.15

Item {
    id: root
    visible: true

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

        // 默认视野：吉林大学中心校区 晏湖 (43.8247°N, 125.2868°E)
        center: QtPositioning.coordinate(43.8247, 125.2868)
        zoomLevel: 18.0
        minimumZoomLevel: 16.0  // 锁定当前晏湖校区范围，防止误缩放到全城
        maximumZoomLevel: 21.0  // 允许超大幅度放大，精细点选航点

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

        // 鼠标交互：滚轮无级缩放 + 左键点选航点 + 鼠标拖拽平移
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
                        mapBridge.addWaypointFromMap(coord.latitude, coord.longitude, 1.2);
                    }
                }
                isDragging = false;
            }

            // 鼠标滚轮放大/缩小地图
            onWheel: {
                if (wheel.angleDelta.y > 0) {
                    map.zoomLevel = Math.min(map.maximumZoomLevel, map.zoomLevel + 0.5);
                } else if (wheel.angleDelta.y < 0) {
                    map.zoomLevel = Math.max(map.minimumZoomLevel, map.zoomLevel - 0.5);
                }
                wheel.accepted = true;
            }
        }
    }

    // 悬浮缩放控制按钮组 (+ / -)
    Column {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 20
        spacing: 8

        Rectangle {
            width: 40
            height: 40
            radius: 8
            color: "#FFFFFF"
            border.color: "#BDBDBD"
            border.width: 1
            opacity: 0.92

            Text {
                anchors.centerIn: parent
                text: "+"
                font.pixelSize: 24
                font.bold: true
                color: "#212121"
            }
            MouseArea {
                anchors.fill: parent
                onClicked: map.zoomLevel = Math.min(map.maximumZoomLevel, map.zoomLevel + 0.5)
            }
        }

        Rectangle {
            width: 40
            height: 40
            radius: 8
            color: "#FFFFFF"
            border.color: "#BDBDBD"
            border.width: 1
            opacity: 0.92

            Text {
                anchors.centerIn: parent
                text: "−"
                font.pixelSize: 24
                font.bold: true
                color: "#212121"
            }
            MouseArea {
                anchors.fill: parent
                onClicked: map.zoomLevel = Math.max(map.minimumZoomLevel, map.zoomLevel - 0.5)
            }
        }
    }

    // 快捷居中按钮：重置晏湖 / 锁定船只
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
                    map.center = QtPositioning.coordinate(43.8247, 125.2868);
                    map.zoomLevel = 18.5;
                }
            }
        }
    }
}
