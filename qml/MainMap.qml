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

        // 默认配置高德卫星/吉林一号或自定义本地离线瓦片服务
        PluginParameter {
            name: "osm.mapping.custom.host"
            value: "https://webst01.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}"
        }
        PluginParameter {
            name: "osm.useragent"
            value: "JLUSV GroundStation"
        }
        PluginParameter {
            name: "osm.mapping.providersrepository.disabled"
            value: true
        }
        PluginParameter {
            name: "osm.mapping.cache.directory"
            value: "./tile_cache"
        }
    }

    Map {
        id: map
        anchors.fill: parent
        plugin: mapPlugin
        center: QtPositioning.coordinate(31.2304, 121.4737) // 默认居中位置，收到数据后自动同步
        zoomLevel: 16
        minimumZoomLevel: 3
        maximumZoomLevel: 19

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
                anchorPoint.x: 12
                anchorPoint.y: 12
                sourceItem: Rectangle {
                    width: 24
                    height: 24
                    radius: 12
                    color: "#FF5252"
                    border.color: "white"
                    border.width: 2
                    Text {
                        anchors.centerIn: parent
                        text: (index + 1).toString()
                        color: "white"
                        font.bold: true
                        font.pixelSize: 11
                    }
                }
            }
        }

        // 船只实时位置与航向图标
        MapQuickItem {
            id: boatMarker
            coordinate: QtPositioning.coordinate(mapBridge.boatLat, mapBridge.boatLon)
            anchorPoint.x: 16
            anchorPoint.y: 16
            visible: mapBridge.boatLat !== 0.0 || mapBridge.boatLon !== 0.0

            sourceItem: Item {
                width: 32
                height: 32
                rotation: mapBridge.boatHeading

                // 船艏三角箭头
                Canvas {
                    anchors.fill: parent
                    onPaint: {
                        var ctx = getContext("2d");
                        ctx.reset();
                        ctx.fillStyle = "#FFEB3B";
                        ctx.strokeStyle = "#D50000";
                        ctx.lineWidth = 2;
                        ctx.beginPath();
                        ctx.moveTo(16, 2);   // 船艏尖端
                        ctx.lineTo(28, 28);  // 右船尾
                        ctx.lineTo(16, 22);  // 尾部凹槽
                        ctx.lineTo(4, 28);   // 左船尾
                        ctx.closePath();
                        ctx.fill();
                        ctx.stroke();
                    }
                }
            }
        }

        // 鼠标打点交互与拖拽地图
        MouseArea {
            id: mouseArea
            anchors.fill: parent
            hoverEnabled: true

            onClicked: {
                var coord = map.toCoordinate(Qt.point(mouse.x, mouse.y));
                if (coord.isValid) {
                    mapBridge.addWaypointFromMap(coord.latitude, coord.longitude, 1.2);
                }
            }
        }
    }

    // 快捷居中按钮
    Rectangle {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 20
        width: 100
        height: 36
        radius: 6
        color: "#2196F3"
        opacity: 0.85

        Text {
            anchors.centerIn: parent
            text: "锁定船只"
            color: "white"
            font.bold: true
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
