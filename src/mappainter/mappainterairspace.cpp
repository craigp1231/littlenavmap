/*****************************************************************************
* Copyright 2015-2020 Alexander Barthel alex@littlenavmap.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "mappainter/mappainterairspace.h"

#include "common/mapcolors.h"
#include "util/paintercontextsaver.h"
#include "route/route.h"
#include "mapgui/maplayer.h"
#include "query/mapquery.h"
#include "airspace/airspacecontroller.h"
#include "navapp.h"
#include "mapgui/mapscale.h"

#include <marble/GeoDataLineString.h>
#include <marble/GeoPainter.h>
#include <marble/GeoDataLinearRing.h>
#include <marble/ViewportParams.h>

#include <QElapsedTimer>

using namespace Marble;
using namespace atools::geo;
using namespace map;

MapPainterAirspace::MapPainterAirspace(MapPaintWidget *mapWidget, MapScale *mapScale, PaintContext *paintContext)
  : MapPainter(mapWidget, mapScale, paintContext)
{
}

MapPainterAirspace::~MapPainterAirspace()
{

}

void MapPainterAirspace::render()
{
  if(!context->mapLayer->isAirspace() || !(context->objectTypes.testFlag(map::AIRSPACE)))
    return;

  if(context->mapLayerEffective->isAirportDiagram())
    return;

  AirspaceController *controller = NavApp::getAirspaceController();

  // Get online and offline airspace and merge then into one list =============
  const GeoDataLatLonAltBox& curBox = context->viewport->viewLatLonAltBox();
  AirspaceVector airspaces;

  if(context->objectTypes.testFlag(map::AIRSPACE))
  {
    // qDebug() << Q_FUNC_INFO << "NON ONLINE";
    controller->getAirspaces(airspaces, curBox, context->mapLayer, context->airspaceFilterByLayer,
                             context->route->getCruisingAltitudeFeet(),
                             context->viewContext == Marble::Animation, map::AIRSPACE_SRC_ALL);
  }

  if(!airspaces.isEmpty())
  {
    Marble::GeoPainter *painter = context->painter;
    atools::util::PainterContextSaver saver(painter);
    Q_UNUSED(saver);

    painter->setBackgroundMode(Qt::TransparentMode);

    for(const MapAirspace *airspace : airspaces)
    {
      if(!(airspace->type & context->airspaceFilterByLayer.types))
        continue;

      if(context->viewportRect.overlaps(airspace->bounding))
      {
        if(context->objCount())
          return;

        // qDebug() << airspace.getId() << airspace.name;

        Marble::GeoDataLinearRing linearRing;
        linearRing.setTessellate(true);

        QPen pen = mapcolors::penForAirspace(*airspace);

        if(airspace->isOnline())
          // Make online airpace line thicker
          pen.setWidthF(pen.widthF() * 2.);

        painter->setPen(pen);

        if(!context->drawFast)
          painter->setBrush(mapcolors::colorForAirspaceFill(*airspace));

        const LineString *lines = controller->getAirspaceGeometry(airspace->combinedId());

        if(lines != nullptr)
        {
          for(const Pos& pos : *lines)
            linearRing.append(Marble::GeoDataCoordinates(pos.getLonX(), pos.getLatY(), 0, DEG));

          painter->drawPolygon(linearRing);
        }

        if(airspace->isOnline())
        {
          // Draw center circle for online airspace with less transparency and darker
          QBrush brush = painter->brush();
          QColor color = brush.color();
          color.setAlphaF(color.alphaF() * 2.f);
          brush.setColor(color.darker(200));
          painter->setBrush(brush);

          // Draw circle with 1 NM and at least 3 pixel radius
          drawCircle(painter, airspace->position, std::max(scale->getPixelForNm(1.f), 3.f));
        }
      }
    }
  }
}
