import QtQuick 2.15
import QtTest 1.2
import QtPositioning 5.15
import "../qml"

Item {
    width: 900
    height: 900

    // Display-only fixture: no GsClient, authentication or device connection.
    QtObject {
        id: mapBridge
        property real boatLat: 43.8253
        property real boatLon: 125.2883
        property real groundStationLat: 43.8253
        property real groundStationLon: 125.2883
        property real boatHeading: 0
        property bool boatHeadingValid: true
        property bool isOnline: true
        property bool groundStationValid: true
        property bool groundReferenceValid: true
        property bool manualGroundStation: false
        property var waypointsQml: []
    }

    MainMap {
        id: mapView
        anchors.fill: parent
    }

    TestCase {
        name: "MapMarkerPosition"
        when: windowShown
        property var navigationMap
        property var boatMarker
        property var groundMarker
        property var boatSymbol

        function initTestCase() {
            navigationMap = findChild(mapView, "navigationMap")
            boatMarker = findChild(mapView, "boatMarker")
            groundMarker = findChild(mapView, "groundStationMarker")
            boatSymbol = findChild(mapView, "boatSymbol")
            verify(navigationMap && boatMarker && groundMarker && boatSymbol)
            tryVerify(function() { return navigationMap.mapReady })
            navigationMap.center = QtPositioning.coordinate(mapBridge.groundStationLat, mapBridge.groundStationLon)
        }

        function test_markerCentre_data() {
            var rows = []
            var positions = [
                {name: "southwest", east: -1, north: -1},
                {name: "southeast", east: 1, north: -1},
                {name: "northwest", east: -1, north: 1},
                {name: "northeast", east: 1, north: 1},
                {name: "coincident", east: 0, north: 0}
            ]
            var headings = [0, 90, 180, 230, 270]
            var zooms = [18.5, 22]
            for (var p = 0; p < positions.length; ++p)
                for (var h = 0; h < headings.length; ++h)
                    for (var z = 0; z < zooms.length; ++z)
                        rows.push({tag: positions[p].name + "-h" + headings[h] + "-z" + zooms[z],
                            east: positions[p].east, north: positions[p].north,
                            heading: headings[h], zoom: zooms[z]})
            return rows
        }

        function test_markerCentre(data) {
            mapBridge.boatHeadingValid = true
            mapBridge.boatHeading = data.heading
            mapBridge.boatLat = mapBridge.groundStationLat + data.north * 0.000009
            mapBridge.boatLon = mapBridge.groundStationLon + data.east * 0.000012
            mapView.applyZoom(data.zoom)
            waitForRendering(mapView)

            var coordinate = QtPositioning.coordinate(mapBridge.boatLat, mapBridge.boatLon)
            var expected = navigationMap.fromCoordinate(coordinate, false)
            var actual = boatSymbol.mapToItem(navigationMap, boatSymbol.width / 2, boatSymbol.height / 2)
            verify(Math.abs(actual.x - expected.x) < 0.01 && Math.abs(actual.y - expected.y) < 0.01,
                "symbol centre " + actual.x + "," + actual.y + " differs from map coordinate " + expected.x + "," + expected.y)

            var gs = groundMarker.sourceItem.mapToItem(navigationMap, groundMarker.anchorPoint.x, groundMarker.anchorPoint.y)
            var gsExpected = navigationMap.fromCoordinate(QtPositioning.coordinate(mapBridge.groundStationLat, mapBridge.groundStationLon), false)
            verify(Math.abs(gs.x - gsExpected.x) < 0.01 && Math.abs(gs.y - gsExpected.y) < 0.01)
            if (data.east !== 0) verify((actual.x - gs.x) * data.east > 0)
            if (data.north !== 0) verify((gs.y - actual.y) * data.north > 0)
            if (data.east === 0 && data.north === 0) {
                verify(Math.abs(actual.x - gs.x) < 0.01 && Math.abs(actual.y - gs.y) < 0.01)
            }
        }

        function test_invalidHeadingRetainsPosition() {
            mapBridge.boatHeadingValid = false
            mapBridge.boatHeading = 230
            waitForRendering(mapView)
            var expected = navigationMap.fromCoordinate(QtPositioning.coordinate(mapBridge.boatLat, mapBridge.boatLon), false)
            var actual = boatSymbol.mapToItem(navigationMap, boatSymbol.width / 2, boatSymbol.height / 2)
            verify(Math.abs(actual.x - expected.x) < 0.01 && Math.abs(actual.y - expected.y) < 0.01)
        }
    }
}
