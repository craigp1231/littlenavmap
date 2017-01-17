/*****************************************************************************
* Copyright 2015-2017 Alexander Barthel albar965@mailbox.org
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

#include "mapgui/mappainterairport.h"

#include "common/symbolpainter.h"
#include "mapgui/mapscale.h"
#include "mapgui/maplayer.h"
#include "mapgui/mapquery.h"
#include "geo/calculations.h"
#include "common/maptypes.h"
#include "common/mapcolors.h"
#include "common/unit.h"
#include "mapgui/mapwidget.h"
#include "route/routecontroller.h"
#include "util/paintercontextsaver.h"

#include <QElapsedTimer>

#include <marble/GeoPainter.h>
#include <marble/ViewportParams.h>

using namespace Marble;
using namespace atools::geo;
using namespace maptypes;

MapPainterAirport::MapPainterAirport(MapWidget *mapWidget, MapQuery *mapQuery, MapScale *mapScale,
                                     RouteController *controller)
  : MapPainter(mapWidget, mapQuery, mapScale), routeController(controller)
{
}

MapPainterAirport::~MapPainterAirport()
{
}

void MapPainterAirport::render(PaintContext *context)
{
  // Get all airports from the route and add them to the map
  QHash<int, const MapAirport *> airportMap; // Collect all airports from route and bounding rectangle
  QSet<int> routeAirportIds; // Airport ids from departure and destination

  if(context->objectTypes.testFlag(maptypes::ROUTE))
  {
    for(const RouteMapObject& rmo : routeController->getRouteMapObjects())
    {
      if(rmo.getMapObjectType() == maptypes::AIRPORT)
      {
        airportMap.insert(rmo.getAirport().id, &rmo.getAirport());
        routeAirportIds.insert(rmo.getAirport().id);
      }
    }
  }

  if((!context->objectTypes.testFlag(maptypes::AIRPORT) || !context->mapLayer->isAirport()) &&
     (!context->mapLayerEffective->isAirportDiagram()) && airportMap.isEmpty())
    return;

  // Get airports from cache/database for the bounding rectangle and add them to the map
  const GeoDataLatLonAltBox& curBox = context->viewport->viewLatLonAltBox();
  const QList<MapAirport> *airportCache = nullptr;
  if(context->mapLayerEffective->isAirportDiagram())
    airportCache = query->getAirports(curBox, context->mapLayerEffective, context->drawFast);
  else
    airportCache = query->getAirports(curBox, context->mapLayer, context->drawFast);

  for(const MapAirport& ap : *airportCache)
    airportMap.insert(ap.id, &ap);

  if(airportMap.isEmpty())
    // Nothing found in bounding rectangle and route
    return;

  setRenderHints(context->painter);

  // Collect all airports that are visible
  QList<const MapAirport *> visibleAirports;
  QList<QPointF> visiblePoints;
  for(const MapAirport *airport : airportMap.values())
  {
    // Either part of the route or enabled in the actions/menus/toolbar
    if(!airport->isVisible(context->objectTypes) && !routeAirportIds.contains(airport->id))
      continue;

    float x, y;
    bool visible = wToS(airport->position, x, y, scale->getScreeenSizeForRect(airport->bounding));

    if(!visible)
      // Check bounding rect for visibility
      visible = airport->bounding.overlaps(context->viewportRect);

    if(visible)
    {
      visibleAirports.append(airport);
      visiblePoints.append(QPointF(x, y));
    }
  }

  if(context->mapLayerEffective->isAirportDiagram())
  {
    // In diagram mode draw background first to avoid overwriting other airports
    for(const MapAirport *airport : visibleAirports)
      drawAirportDiagramBackround(context, *airport);
  }

  // Draw the diagrams first
  for(int i = 0; i < visibleAirports.size(); i++)
  {
    const MapAirport *airport = visibleAirports.at(i);

    // Airport diagram is not influenced by detail level
    if(context->mapLayerEffective->isAirportDiagram())
      drawAirportDiagram(context, *airport, context->drawFast);
  }

  // Add airport symbols on top of diagrams
  for(int i = 0; i < visibleAirports.size(); i++)
  {
    const MapAirport *airport = visibleAirports.at(i);
    const QPointF& pt = visiblePoints.at(i);
    const MapLayer *layer = context->mapLayer;

    // Airport diagram is not influenced by detail level
    if(!context->mapLayerEffective->isAirportDiagram())
      drawAirportSymbolOverview(context, *airport);

    // More detailed symbol will be drawn by the route painter - so skip here
    if(!routeAirportIds.contains(airport->id))
    {
      // if(context->dOpt(opts::ITEM_AIRPORT_WIND_POINTER))
      // drawWindPointer(context, *airport, pt.x(), pt.y());

      // Symbol will be omitted for runway overview
      drawAirportSymbol(context, *airport, pt.x(), pt.y());

      // Build and draw airport text
      textflags::TextFlags flags;

      if(layer->isAirportInfo())
        flags = textflags::IDENT | textflags::NAME | textflags::INFO;

      if(layer->isAirportIdent())
        flags |= textflags::IDENT;
      else if(layer->isAirportName())
        flags |= textflags::NAME;

      context->szFont(context->textSizeAirport);
      symbolPainter->drawAirportText(context->painter, *airport, pt.x(), pt.y(), context->dispOpts,
                                     flags,
                                     context->sz(context->symbolSizeAirport,
                                                 context->mapLayerEffective->getAirportSymbolSize()),
                                     context->mapLayerEffective->isAirportDiagram());
    }
  }
}

/* Draws the full airport diagram including runway, taxiways, apron, parking and more */
void MapPainterAirport::drawAirportDiagramBackround(const PaintContext *context,
                                                    const maptypes::MapAirport& airport)
{
  Marble::GeoPainter *painter = context->painter;
  atools::util::PainterContextSaver saver(painter);
  painter->setBackgroundMode(Qt::OpaqueMode);
  painter->setFont(context->defaultFont);

  painter->setBrush(mapcolors::airportDetailBackColor);
  // Build a thick pen of around 200 meters to draw the airport background
  painter->setPen(QPen(mapcolors::airportDetailBackColor,
                       scale->getPixelIntForMeter(AIRPORT_DIAGRAM_BACKGROUND_METER),
                       Qt::SolidLine, Qt::RoundCap));

  // Get all runways for this airport
  const QList<MapRunway> *runways = query->getRunways(airport.id);

  // Calculate all runway screen coordinates
  QList<QPoint> runwayCenters;
  QList<QRect> runwayRects, runwayOutlineRects;
  runwayCoords(runways, &runwayCenters, &runwayRects, nullptr, &runwayOutlineRects);

  // Draw white background ---------------------------------
  // For runways
  for(int i = 0; i < runwayCenters.size(); i++)
    if(runways->at(i).surface != "W")
    {
      painter->translate(runwayCenters.at(i));
      painter->rotate(runways->at(i).heading);

      const QRect backRect = runwayOutlineRects.at(i);
      painter->drawRect(backRect);

      painter->resetTransform();
    }

  // For taxipaths
  const QList<MapTaxiPath> *taxipaths = query->getTaxiPaths(airport.id);
  for(const MapTaxiPath& taxipath : *taxipaths)
  {
    bool visible;
    QPoint start = wToS(taxipath.start, DEFAULT_WTOS_SIZE, &visible);
    QPoint end = wToS(taxipath.end, DEFAULT_WTOS_SIZE, &visible);
    painter->drawLine(start, end);
  }

  // For aprons
  const QList<MapApron> *aprons = query->getAprons(airport.id);
  QVector<QPoint> points;
  for(const MapApron& apron : *aprons)
  {
    points.clear();
    bool visible;
    for(const Pos& pos : apron.vertices)
      points.append(wToS(pos, DEFAULT_WTOS_SIZE, &visible));

    painter->QPainter::drawPolyline(points.data(), points.size());
  }
}

/* Draws the full airport diagram including runway, taxiways, apron, parking and more */
void MapPainterAirport::drawAirportDiagram(const PaintContext *context, const maptypes::MapAirport& airport,
                                           bool fast)
{
  Marble::GeoPainter *painter = context->painter;
  atools::util::PainterContextSaver saver(painter);
  painter->setBackgroundMode(Qt::OpaqueMode);
  painter->setFont(context->defaultFont);

  // Get all runways for this airport
  const QList<MapRunway> *runways = query->getRunways(airport.id);

  // Calculate all runway screen coordinates
  QList<QPoint> runwayCenters;
  QList<QRect> runwayRects, runwayOutlineRects;
  runwayCoords(runways, &runwayCenters, &runwayRects, nullptr, &runwayOutlineRects);

  // Draw aprons ---------------------------------
  painter->setBackground(Qt::transparent);
  const QList<MapApron> *aprons = query->getAprons(airport.id);
  QVector<QPoint> points;
  for(const MapApron& apron : *aprons)
  {
    points.clear();
    bool visible;
    for(const Pos& pos : apron.vertices)
      points.append(wToS(pos, DEFAULT_WTOS_SIZE, &visible));

    // Draw aprons a bit darker so we can see the taxiways
    QColor col = mapcolors::colorForSurface(apron.surface);
    col = col.darker(110);

    painter->setPen(QPen(col, 1, Qt::SolidLine, Qt::FlatCap));

    if(!apron.drawSurface)
      // Use pattern for transparent aprons
      painter->setBrush(QBrush(col, Qt::Dense6Pattern));
    else
      painter->setBrush(QBrush(col));

    painter->QPainter::drawPolygon(points.data(), points.size());
  }

  // Draw taxiways ---------------------------------
  painter->setBackgroundMode(Qt::OpaqueMode);
  QVector<QPoint> startPts, endPts;
  QVector<int> pathThickness;

  // Collect coordinates first
  const QList<MapTaxiPath> *taxipaths = query->getTaxiPaths(airport.id);
  for(const MapTaxiPath& taxipath : *taxipaths)
  {
    bool visible;
    // Do not do any clipping here
    startPts.append(wToS(taxipath.start, DEFAULT_WTOS_SIZE, &visible));
    endPts.append(wToS(taxipath.end, DEFAULT_WTOS_SIZE, &visible));
    pathThickness.append(std::max(2, scale->getPixelIntForFeet(taxipath.width)));
  }

  // Draw closed and others first to have real taxiways on top
  for(int i = 0; i < taxipaths->size(); i++)
  {
    const MapTaxiPath& taxipath = taxipaths->at(i);
    int thickness = pathThickness.at(i);
    QColor col = mapcolors::colorForSurface(taxipath.surface);

    const QPoint& start = startPts.at(i);
    const QPoint& end = endPts.at(i);

    if(taxipath.closed)
    {
      painter->setPen(QPen(col, thickness, Qt::SolidLine, Qt::RoundCap));
      painter->drawLine(start, end);

      painter->setPen(QPen(mapcolors::taxiwayClosedBrush, thickness, Qt::SolidLine, Qt::RoundCap));
      painter->drawLine(start, end);

    }
    else if(!taxipath.drawSurface)
    {
      painter->setPen(QPen(QBrush(col, Qt::Dense4Pattern), thickness, Qt::SolidLine, Qt::RoundCap));
      painter->drawLine(start, end);
    }
  }

  for(int i = 0; i < taxipaths->size(); i++)
  {
    const MapTaxiPath& taxipath = taxipaths->at(i);
    if(!taxipath.closed && taxipath.drawSurface)
    {
      painter->setPen(QPen(mapcolors::colorForSurface(taxipath.surface),
                           pathThickness.at(i), Qt::SolidLine, Qt::RoundCap));
      painter->drawLine(startPts.at(i), endPts.at(i));
    }
  }

  // Draw taxiway names ---------------------------------
  if(!fast && context->mapLayerEffective->isAirportDiagramDetail())
  {
    QFontMetrics taxiMetrics = painter->fontMetrics();
    painter->setBackgroundMode(Qt::TransparentMode);
    painter->setPen(QPen(mapcolors::taxiwayNameColor, 2, Qt::SolidLine, Qt::FlatCap));

    // Map all visible names to paths
    QMultiMap<QString, MapTaxiPath> map;
    for(const MapTaxiPath& taxipath : *taxipaths)
    {
      if(!taxipath.name.isEmpty())
      {
        bool visible;
        wToS(taxipath.start, DEFAULT_WTOS_SIZE, &visible);
        wToS(taxipath.end, DEFAULT_WTOS_SIZE, &visible);
        if(visible)
          map.insert(taxipath.name, taxipath);
      }
    }

    for(QString taxiname : map.keys())
    {
      QList<MapTaxiPath> paths = map.values(taxiname);
      QList<MapTaxiPath> pathsToLabel;

      // Simplified text placement - take first, last and middle name for a path
      pathsToLabel.append(paths.first());
      if(paths.size() > 2)
        pathsToLabel.append(paths.at(paths.size() / 2));
      pathsToLabel.append(paths.last());

      for(const MapTaxiPath& taxipath : pathsToLabel)
      {
        bool visible;
        QPoint start = wToS(taxipath.start, DEFAULT_WTOS_SIZE, &visible);
        QPoint end = wToS(taxipath.end, DEFAULT_WTOS_SIZE, &visible);

        QRect textrect = taxiMetrics.boundingRect(taxiname);

        int length = atools::geo::simpleDistance(start.x(), start.y(), end.x(), end.y());
        if(length > TAXIWAY_TEXT_MIN_LENGTH)
        {
          // Only draw is segment is longer than 40 pixels
          int x = ((start.x() + end.x()) / 2) - textrect.width() / 2;
          int y = ((start.y() + end.y()) / 2) + textrect.height() / 2 - taxiMetrics.descent();
          textrect.moveTo(x, y - textrect.height() + taxiMetrics.descent());
          textrect.adjust(-1, -1, 1, 1);
          painter->fillRect(textrect, mapcolors::taxiwayNameBackgroundColor);
          painter->drawText(x, y, taxiname);
        }
      }
    }
    painter->setBackgroundMode(Qt::OpaqueMode);
  }

  if(!fast)
  {
    // Draw runway overrun and blast pads --------------------------------
    painter->setPen(QPen(mapcolors::runwayOutlineColor, 1, Qt::SolidLine, Qt::FlatCap));
    for(int i = 0; i < runwayCenters.size(); i++)
    {
      const MapRunway& runway = runways->at(i);
      const QRect& rect = runwayRects.at(i);
      QColor col = mapcolors::colorForSurface(runway.surface);

      painter->translate(runwayCenters.at(i));
      painter->rotate(runways->at(i).heading);

      // Draw overrun areas
      if(runway.primaryOverrun > 0)
      {
        int offs = scale->getPixelIntForFeet(runway.primaryOverrun, runway.heading);
        painter->setBrush(mapcolors::runwayOverrunBrush);
        painter->setBackground(col);
        painter->drawRect(rect.left(), rect.bottom(), rect.width(), offs);
      }
      if(runway.secondaryOverrun > 0)
      {
        int offs = scale->getPixelIntForFeet(runway.secondaryOverrun, runway.heading);
        painter->setBrush(mapcolors::runwayOverrunBrush);
        painter->setBackground(col);
        painter->drawRect(rect.left(), rect.top() - offs, rect.width(), offs);
      }

      // Draw blast pads
      if(runway.primaryBlastPad > 0)
      {
        int offs = scale->getPixelIntForFeet(runway.primaryBlastPad, runway.heading);
        painter->setBrush(mapcolors::runwayBlastpadBrush);
        painter->setBackground(col);
        painter->drawRect(rect.left(), rect.bottom(), rect.width(), offs);
      }
      if(runway.secondaryBlastPad > 0)
      {
        int offs = scale->getPixelIntForFeet(runway.secondaryBlastPad, runway.heading);
        painter->setBrush(mapcolors::runwayBlastpadBrush);
        painter->setBackground(col);
        painter->drawRect(rect.left(), rect.top() - offs, rect.width(), offs);
      }

      painter->resetTransform();
    }
  }

  if(!fast)
  {
    // Draw black runway outlines --------------------------------
    painter->setPen(QPen(mapcolors::runwayOutlineColor, 1, Qt::SolidLine, Qt::FlatCap));
    painter->setBrush(mapcolors::runwayOutlineColor);
    for(int i = 0; i < runwayCenters.size(); i++)
    {
      if(runways->at(i).surface != "W")
      {
        painter->translate(runwayCenters.at(i));
        painter->rotate(runways->at(i).heading);
        painter->drawRect(runwayRects.at(i).marginsAdded(QMargins(2, 2, 2, 2)));
        painter->resetTransform();
      }
    }
  }

  // Draw runways --------------------------------
  for(int i = 0; i < runwayCenters.size(); i++)
  {
    const MapRunway& runway = runways->at(i);
    const QRect& rect = runwayRects.at(i);

    QColor col = mapcolors::colorForSurface(runway.surface);

    painter->translate(runwayCenters.at(i));
    painter->rotate(runway.heading);

    painter->setBrush(col);
    painter->setPen(QPen(col, 1, Qt::SolidLine, Qt::FlatCap));
    painter->drawRect(rect);
    painter->resetTransform();
  }

  if(!fast)
  {
    // Draw runway offset thresholds --------------------------------
    painter->setBackgroundMode(Qt::TransparentMode);
    painter->setBrush(mapcolors::runwayOffsetColor);
    for(int i = 0; i < runwayCenters.size(); i++)
    {
      const MapRunway& runway = runways->at(i);

      if(runway.primaryOffset > 0 || runway.secondaryOffset > 0)
      {
        const QRect& rect = runwayRects.at(i);

        painter->translate(runwayCenters.at(i));
        painter->rotate(runway.heading);

        if(runway.primaryOffset > 0)
        {
          int offs = scale->getPixelIntForFeet(runway.primaryOffset, runway.heading);

          // Draw solid boundary to runway
          painter->setPen(QPen(mapcolors::runwayOffsetColor, 3, Qt::SolidLine, Qt::FlatCap));
          painter->drawLine(rect.left(), rect.bottom() - offs, rect.right(), rect.bottom() - offs);

          // Draw dashed line
          painter->setPen(QPen(mapcolors::runwayOffsetColor, 3, Qt::DashLine, Qt::FlatCap));
          painter->drawLine(0, rect.bottom(), 0, rect.bottom() - offs);
        }

        if(runway.secondaryOffset > 0)
        {
          int offs = scale->getPixelIntForFeet(runway.secondaryOffset, runway.heading);

          // Draw solid boundary to runway
          painter->setPen(QPen(mapcolors::runwayOffsetColor, 3, Qt::SolidLine, Qt::FlatCap));
          painter->drawLine(rect.left(), rect.top() + offs, rect.right(), rect.top() + offs);

          // Draw dashed line
          painter->setPen(QPen(mapcolors::runwayOffsetColor, 3, Qt::DashLine, Qt::FlatCap));
          painter->drawLine(0, rect.top(), 0, rect.top() + offs);
        }
        painter->resetTransform();
      }
    }
    painter->setBackgroundMode(Qt::OpaqueMode);
  }

  // Draw parking --------------------------------
  const QList<MapParking> *parkings = query->getParkingsForAirport(airport.id);
  if(!parkings->isEmpty())
    painter->setPen(QPen(mapcolors::parkingOutlineColor, 2, Qt::SolidLine, Qt::FlatCap));
  for(const MapParking& parking : *parkings)
  {
    bool visible;
    QPoint pt = wToS(parking.position, DEFAULT_WTOS_SIZE, &visible);
    if(visible)
    {
      // Calculate approximate screen width and height
      int w = scale->getPixelIntForFeet(parking.radius, 90);
      int h = scale->getPixelIntForFeet(parking.radius, 0);

      painter->setBrush(mapcolors::colorForParkingType(parking.type));
      painter->drawEllipse(pt, w, h);

      if(!fast)
      {
        if(parking.jetway)
          // Draw second ring for jetway
          painter->drawEllipse(pt, w * 3 / 4, h * 3 / 4);

        // Draw heading tick mark
        painter->translate(pt);
        painter->rotate(parking.heading);
        painter->drawLine(0, h * 2 / 3, 0, h);
        painter->resetTransform();
      }
    }
  }

  // Draw helipads ------------------------------------------------
  const QList<MapHelipad> *helipads = query->getHelipads(airport.id);
  if(!helipads->isEmpty())
  {
    for(const MapHelipad& helipad : *helipads)
    {
      bool visible;
      QPoint pt = wToS(helipad.position, DEFAULT_WTOS_SIZE, &visible);
      if(visible)
      {
        painter->setBrush(mapcolors::colorForSurface(helipad.surface));

        int w = scale->getPixelIntForFeet(helipad.width, 90) / 2;
        int h = scale->getPixelIntForFeet(helipad.length, 0) / 2;

        painter->setPen(QPen(mapcolors::helipadOutlineColor, 2, Qt::SolidLine, Qt::FlatCap));

        painter->translate(pt);
        painter->rotate(helipad.heading);

        if(helipad.type == "SQUARE" || helipad.type == "MEDICAL")
          painter->drawRect(-w, -h, w * 2, h * 2);
        else
          painter->drawEllipse(-w, -h, w * 2, h * 2);

        if(!fast)
        {
          if(helipad.type == "MEDICAL")
            painter->setPen(QPen(mapcolors::helipadMedicalOutlineColor, 3, Qt::SolidLine, Qt::FlatCap));

          // if(helipad.type != "CIRCLE")
          // {
          // Draw the H symbol
          painter->drawLine(-w / 3, -h / 2, -w / 3, h / 2);
          painter->drawLine(-w / 3, 0, w / 3, 0);
          painter->drawLine(w / 3, -h / 2, w / 3, h / 2);
          // }

          if(helipad.closed)
          {
            // Cross out
            painter->drawLine(-w, -w, w, w);
            painter->drawLine(-w, w, w, -w);
          }
        }
        painter->resetTransform();
      }
    }
  }

  // Draw tower -------------------------------------------------
  if(airport.towerCoords.isValid())
  {
    bool visible;
    QPoint pt = wToS(airport.towerCoords, DEFAULT_WTOS_SIZE, &visible);
    if(visible)
    {
      if(airport.towerFrequency > 0)
      {
        painter->setPen(QPen(mapcolors::activeTowerOutlineColor, 2, Qt::SolidLine, Qt::FlatCap));
        painter->setBrush(mapcolors::activeTowerColor);
      }
      else
      {
        painter->setPen(QPen(mapcolors::inactiveTowerOutlineColor, 2, Qt::SolidLine, Qt::FlatCap));
        painter->setBrush(mapcolors::inactiveTowerColor);
      }

      int w = scale->getPixelIntForMeter(10, 90);
      int h = scale->getPixelIntForMeter(10, 0);
      painter->drawEllipse(pt, w < 6 ? 6 : w, h < 6 ? 6 : h);
    }
  }

  painter->setBackgroundMode(Qt::TransparentMode);

  // Draw texts ------------------------------------------------------------------

  // Draw parking and tower texts -------------------------------------------------
  QFontMetrics metrics = painter->fontMetrics();
  if(!fast && context->mapLayerEffective->isAirportDiagramDetail())
  {
    for(const MapParking& parking : *parkings)
      if(context->mapLayerEffective->isAirportDiagramDetail2() || parking.radius > 40)
      {
        bool visible;
        QPoint pt = wToS(parking.position, DEFAULT_WTOS_SIZE, &visible);
        if(visible)
        {
          // Use different text pen for better readability depending on background
          if(parking.type.startsWith("RAMP_GA") || parking.type.startsWith("DOCK_GA") ||
             parking.type.startsWith("FUEL"))
            painter->setPen(QPen(mapcolors::darkParkingTextColor, 2, Qt::SolidLine, Qt::FlatCap));
          else
            painter->setPen(QPen(mapcolors::brightParkingTextColor, 2, Qt::SolidLine, Qt::FlatCap));

          QString text;
          if(parking.type.startsWith("FUEL"))
            text = tr("F");
          else
            text = QString::number(parking.number) + " " + parkingTypeName(parking.name);

          pt.setY(pt.y() + metrics.ascent() / 2);
          pt.setX(pt.x() - metrics.width(text) / 2);

          painter->drawText(pt, text);
        }
      }
  }

  // Draw tower T -----------------------------
  if(!fast && airport.towerCoords.isValid())
  {
    bool visible;
    QPoint pt = wToS(airport.towerCoords, DEFAULT_WTOS_SIZE, &visible);
    if(visible)
    {
      pt.setY(pt.y() + metrics.ascent() / 2);
      pt.setX(pt.x() - metrics.width(tr("T")) / 2);
      painter->setPen(QPen(mapcolors::towerTextColor, 2, Qt::SolidLine, Qt::FlatCap));
      painter->drawText(pt, tr("T"));
    }
  }

  // Draw runway texts --------------------------------
  if(!fast)
  {
    QFont rwTextFont = context->defaultFont;
    rwTextFont.setPixelSize(RUNWAY_TEXT_FONT_SIZE);
    painter->setFont(rwTextFont);
    QFontMetrics rwMetrics = painter->fontMetrics();

    QVector<int> runwayTextLengths;
    // Draw dimensions at runway side
    painter->setPen(QPen(mapcolors::runwayDimsTextColor, 3, Qt::SolidLine, Qt::FlatCap));
    for(int i = 0; i < runwayCenters.size(); i++)
    {
      const MapRunway& runway = runways->at(i);
      const QRect& runwayRect = runwayRects.at(i);

      QString text = QString::number(Unit::distShortFeetF(runway.length), 'f', 0);

      if(runway.width > 8)
        // Skip dummy lines where the runway is done by photo scenery or similar
        text += tr(" x ") + QString::number(Unit::distShortFeetF(runway.width), 'f', 0);
      text += " " + Unit::getUnitShortDistStr();

      // Add light indicator
      if(!runway.edgeLight.isEmpty())
        text += tr(" / L");

      QString surface = maptypes::surfaceName(runway.surface);
      if(!surface.isEmpty())
        text += tr(" / ") + surface;

      int textWidth = rwMetrics.width(text);
      if(textWidth > runwayRect.height())
        textWidth = runwayRect.height();

      int textx = -textWidth / 2, texty = -runwayRect.width() / 2;

      runwayTextLengths.append(textWidth);
      // Truncate text to runway length
      text = rwMetrics.elidedText(text, Qt::ElideRight, runwayRect.height());

      painter->translate(runwayCenters.at(i));
      if(runway.heading > 180.f)
        painter->rotate(runway.heading + 90.f);
      else
        painter->rotate(runway.heading - 90.f);

      // Draw semi-transparent rectangle behind text
      QRect textBackRect = rwMetrics.boundingRect(text);
      textBackRect.moveTo(textx, texty - textBackRect.height() - 5);
      painter->fillRect(textBackRect, mapcolors::runwayTextBackgroundColor);

      // Draw runway length x width / L / surface
      painter->drawText(textx, texty - rwMetrics.descent() - 5, text);
      painter->resetTransform();
    }

    // Draw runway heading with arrow at the side
    QFont rwHdgTextFont = painter->font();
    rwHdgTextFont.setPixelSize(RUNWAY_HEADING_FONT_SIZE);
    painter->setFont(rwHdgTextFont);
    QFontMetrics rwHdgMetrics = painter->fontMetrics();

    for(int i = 0; i < runwayCenters.size(); i++)
    {
      const MapRunway& runway = runways->at(i);
      const QRect& runwayRect = runwayRects.at(i);
      float magHeading = normalizeCourse(runway.heading - airport.magvar);

      QString textPrim;
      QString textSec;

      float rotate;
      if(runway.heading > 180.f)
      {
        // This case is rare (eg. LTAI) - probably primary in the wrong place
        rotate = runway.heading + 90.f;
        textPrim = QString(tr("► ") +
                           QString::number(normalizeCourse(opposedCourseDeg(magHeading)), 'f', 0) + tr("°M"));
        textSec = QString(QString::number(magHeading, 'f', 0) + tr("°M ◄"));
      }
      else
      {
        rotate = runway.heading - 90.f;
        textPrim = QString(tr("► ") + QString::number(magHeading, 'f', 0) + tr("°M"));
        textSec = QString(QString::number(
                            normalizeCourse(opposedCourseDeg(magHeading)), 'f', 0) + tr("°M ◄"));
      }

      QRect textRectPrim = rwHdgMetrics.boundingRect(textPrim);
      QRect textRectSec = rwHdgMetrics.boundingRect(textSec);

      if(textRectPrim.width() + textRectSec.width() + runwayTextLengths.at(i) < runwayRect.height())
      {
        // If all texts fit along the runway side draw heading
        painter->translate(runwayCenters.at(i));
        painter->rotate(rotate);

        textRectPrim.moveTo(-runwayRect.height() / 2, -runwayRect.width() / 2 - textRectPrim.height() - 5);
        painter->fillRect(textRectPrim, mapcolors::runwayTextBackgroundColor);
        painter->drawText(-runwayRect.height() / 2,
                          -runwayRect.width() / 2 - rwHdgMetrics.descent() - 5, textPrim);

        textRectSec.moveTo(runwayRect.height() / 2 - textRectSec.width(),
                           -runwayRect.width() / 2 - textRectSec.height() - 5);
        painter->fillRect(textRectSec, mapcolors::runwayTextBackgroundColor);
        painter->drawText(runwayRect.height() / 2 - textRectSec.width(),
                          -runwayRect.width() / 2 - rwHdgMetrics.descent() - 5, textSec);
        painter->resetTransform();
      }
    }

    // Draw runway numbers at end
    rwTextFont.setPixelSize(RUNWAY_NUMBER_FONT_SIZE);
    painter->setFont(rwTextFont);
    QFontMetrics rwTextMetrics = painter->fontMetrics();
    const int CROSS_SIZE = 10;
    for(int i = 0; i < runwayCenters.size(); i++)
    {
      const MapRunway& runway = runways->at(i);
      bool primaryVisible, secondaryVisible;
      QPoint prim = wToS(runway.primaryPosition, DEFAULT_WTOS_SIZE, &primaryVisible);
      QPoint sec = wToS(runway.secondaryPosition, DEFAULT_WTOS_SIZE, &secondaryVisible);

      // TODO why is primary and secondary reversed - check database
      if(primaryVisible)
      {
        painter->translate(prim);
        painter->rotate(runway.heading + 180.f);

        QRect rectSec = rwTextMetrics.boundingRect(runway.secondaryName);
        rectSec.moveTo(-rectSec.width() / 2, 4);

        painter->fillRect(rectSec, mapcolors::runwayTextBackgroundColor);
        painter->drawText(-rectSec.width() / 2, rwTextMetrics.ascent() + 4, runway.secondaryName);

        if(runway.secondaryClosed)
        {
          // Cross out runway number
          painter->drawLine(-CROSS_SIZE + 4, -CROSS_SIZE + 10 + 4, CROSS_SIZE + 4, CROSS_SIZE + 10 + 4);
          painter->drawLine(-CROSS_SIZE + 4, CROSS_SIZE + 10 + 4, CROSS_SIZE + 4, -CROSS_SIZE + 10 + 4);
        }
        painter->resetTransform();
      }

      if(secondaryVisible)
      {
        painter->translate(sec);
        painter->rotate(runway.heading);

        QRect rectPrim = rwTextMetrics.boundingRect(runway.primaryName);
        rectPrim.moveTo(-rectPrim.width() / 2, 4);

        painter->fillRect(rectPrim, mapcolors::runwayTextBackgroundColor);
        painter->drawText(-rectPrim.width() / 2, rwTextMetrics.ascent() + 4, runway.primaryName);

        if(runway.primaryClosed)
        {
          // Cross out runway number
          painter->drawLine(-CROSS_SIZE + 4, -CROSS_SIZE + 10 + 4, CROSS_SIZE + 4, CROSS_SIZE + 10 + 4);
          painter->drawLine(-CROSS_SIZE + 4, CROSS_SIZE + 10 + 4, CROSS_SIZE + 4, -CROSS_SIZE + 10 + 4);
        }
        painter->resetTransform();
      }
    }
  }
}

/* Draw airport runway overview as in VFR maps (runways with white center line) */
void MapPainterAirport::drawAirportSymbolOverview(const PaintContext *context, const maptypes::MapAirport& ap)
{
  Marble::GeoPainter *painter = context->painter;

  if(ap.longestRunwayLength >= RUNWAY_OVERVIEW_MIN_LENGTH_FEET &&
     context->mapLayerEffective->isAirportOverviewRunway() &&
     !ap.flags.testFlag(maptypes::AP_CLOSED) && !ap.waterOnly())
  {
    // Draw only for airports with a runway longer than 8000 feet otherwise use symbol
    atools::util::PainterContextSaver saver(painter);

    QColor apColor = mapcolors::colorForAirport(ap);
    painter->setBackgroundMode(Qt::OpaqueMode);

    // Get all runways longer than 4000 feet
    const QList<maptypes::MapRunway> *rw = query->getRunwaysForOverview(ap.id);

    QList<QPoint> centers;
    QList<QRect> rects, innerRects;
    runwayCoords(rw, &centers, &rects, &innerRects, nullptr);

    // Draw outline in airport color (magenta or green depending on tower)
    painter->setBrush(QBrush(apColor));
    painter->setPen(QPen(QBrush(apColor), 1, Qt::SolidLine, Qt::FlatCap));
    for(int i = 0; i < centers.size(); i++)
    {
      painter->translate(centers.at(i));
      painter->rotate(rw->at(i).heading);
      painter->drawRect(rects.at(i));
      painter->resetTransform();
    }

    if(!context->drawFast || context->mapLayerEffective->isAirportDiagram())
    {
      // Draw white center lines
      painter->setPen(QPen(QBrush(mapcolors::airportSymbolFillColor), 1, Qt::SolidLine, Qt::FlatCap));
      painter->setBrush(QBrush(mapcolors::airportSymbolFillColor));
      for(int i = 0; i < centers.size(); i++)
      {
        painter->translate(centers.at(i));
        painter->rotate(rw->at(i).heading);
        painter->drawRect(innerRects.at(i));
        painter->resetTransform();
      }
    }
  }
}

/* Draws the airport symbol. This is not drawn if the airport is drawn using runway overview */
void MapPainterAirport::drawAirportSymbol(PaintContext *context, const maptypes::MapAirport& ap,
                                          float x, float y)
{
  if(!context->mapLayerEffective->isAirportOverviewRunway() || ap.flags.testFlag(maptypes::AP_CLOSED) ||
     ap.waterOnly() || ap.longestRunwayLength < RUNWAY_OVERVIEW_MIN_LENGTH_FEET ||
     context->mapLayerEffective->isAirportDiagram())
  {
    if(context->objCount())
      return;

    int size = context->sz(context->symbolSizeAirport, context->mapLayerEffective->getAirportSymbolSize());
    bool isAirportDiagram = context->mapLayerEffective->isAirportDiagram();

    symbolPainter->drawAirportSymbol(context->painter, ap, x, y, size, isAirportDiagram, context->drawFast);
  }
}

// void MapPainterAirport::drawWindPointer(const PaintContext *context, const MapAirport& ap, int x, int y)
// {
// Q_UNUSED(ap);
// const atools::fs::sc::SimConnectUserAircraft& aircraft = mapWidget->getUserAircraft();
// if(aircraft.getPosition().isValid())
// {
// int size = context->sz(context->symbolSizeAirport,
// context->mapLayerEffective->getAirportSymbolSize() * 2);
// symbolPainter->drawWindPointer(context->painter, QColor(), x, y, size, true, context->drawFast);
// }
// }

/*
 * Fill coordinate arrays for all runways of an airport.
 * @param runways runway input object
 * @param centers center points
 * @param rects Runway rectangles
 * @param innerRects Fill rectangles
 * @param outlineRects Big white outline
 */
void MapPainterAirport::runwayCoords(const QList<maptypes::MapRunway> *runways, QList<QPoint> *centers,
                                     QList<QRect> *rects, QList<QRect> *innerRects,
                                     QList<QRect> *outlineRects)
{
  for(const maptypes::MapRunway& r : *runways)
  {
    Rect bounding(r.primaryPosition);
    bounding.extend(r.secondaryPosition);
    QSize size = scale->getScreeenSizeForRect(bounding);

    // Get the two endpoints as screen coords
    float xr1, yr1, xr2, yr2;
    wToS(r.primaryPosition, xr1, yr1, size);
    wToS(r.secondaryPosition, xr2, yr2, size);

    // Get the center point as screen coords
    float xc, yc;
    wToS(r.position, xc, yc, size);
    if(centers != nullptr)
      centers->append(QPoint(static_cast<int>(std::round(xc)), static_cast<int>(std::round(yc))));

    // Get an approximation of the runway length on the screen
    int length = atools::geo::simpleDistance(xr1, yr1, xr2, yr2);

    int width = 6;
    if(r.width > 0)
      // Get an approximation of the runway width on the screen
      width = scale->getPixelIntForFeet(r.width, r.heading + 90.f);

    int backgroundSize = scale->getPixelIntForMeter(AIRPORT_DIAGRAM_BACKGROUND_METER);

    if(outlineRects != nullptr)
      outlineRects->append(QRect(-width - backgroundSize, -length / 2 - backgroundSize,
                                 width + backgroundSize * 2, length + backgroundSize * 2));

    if(rects != nullptr)
      rects->append(QRect(-(width / 2), -length / 2, width, length));

    if(innerRects != nullptr)
      innerRects->append(QRect(-(width / 6), -length / 2 + 2, width - 4, length - 4));
  }
}
