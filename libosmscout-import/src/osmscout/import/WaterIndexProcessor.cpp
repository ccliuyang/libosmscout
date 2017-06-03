/*
  This source is part of the libosmscout library
  Copyright (C) 2017  Tim Teulings

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
*/

#include <osmscout/import/WaterIndexProcessor.h>

#include <iostream>
#include <iomanip>

#include <osmscout/TypeFeatures.h>
#include <osmscout/WaterIndex.h>

#include <osmscout/system/Assert.h>
#include <osmscout/system/Math.h>

#include <osmscout/util/String.h>
#include <osmscout/util/StopClock.h>
#include <osmscout/util/Geometry.h>

#if !defined(DEBUG_COASTLINE)
//#define DEBUG_COASTLINE
#endif

#if !defined(DEBUG_TILING)
//#define DEBUG_TILING
#endif

namespace osmscout {

  void WriteGpx(const std::vector<Point> &path, const std::string& name)
  {
    WriteGpx(path.begin(), path.end(), name);
  }

  /**
   * Sets the size of the bitmap and initializes state of all tiles to "unknown"
   */
  void WaterIndexProcessor::StateMap::SetBox(const GeoBox& boundingBox,
                                             double cellWidth,
                                             double cellHeight)
  {
    const GeoCoord minCoord=boundingBox.GetMinCoord();
    const GeoCoord maxCoord=boundingBox.GetMaxCoord();

    this->cellWidth=cellWidth;
    this->cellHeight=cellHeight;

    cellXStart=(uint32_t)floor((minCoord.GetLon()+180.0)/cellWidth);
    cellXEnd=(uint32_t)floor((maxCoord.GetLon()+180.0)/cellWidth);
    cellYStart=(uint32_t)floor((minCoord.GetLat()+90.0)/cellHeight);
    cellYEnd=(uint32_t)floor((maxCoord.GetLat()+90.0)/cellHeight);

    cellXCount=cellXEnd-cellXStart+1;
    cellYCount=cellYEnd-cellYStart+1;

    uint32_t size=(cellXCount*cellYCount)/4;

    if ((cellXCount*cellYCount)%4>0) {
      size++;
    }

    area.resize(size,0x00);
  }

  WaterIndexProcessor::State WaterIndexProcessor::StateMap::GetState(uint32_t x, uint32_t y) const
  {
    uint32_t cellId=y*cellXCount+x;
    uint32_t index=cellId/4;
    uint32_t offset=2*(cellId%4);

    //assert(index<area.size());

    return (State)((area[index] >> offset) & 3);
  }

  void WaterIndexProcessor::StateMap::SetState(uint32_t x, uint32_t y, State state)
  {
    uint32_t cellId=y*cellXCount+x;
    uint32_t index=cellId/4;
    uint32_t offset=2*(cellId%4);

    //assert(index<area.size());

    area[index]=(area[index] & ~(3 << offset));
    area[index]=(area[index] | (state << offset));
  }

  std::string WaterIndexProcessor::StateToString(State state) const
  {
    switch (state) {
    case unknown:
      return "unknown";
    case land:
      return "land";
    case water:
      return "water";
    case coast:
      return "coast";
    default:
      return "???";
    }
  }

  std::string WaterIndexProcessor::TypeToString(GroundTile::Type type) const
  {
    switch (type) {
    case GroundTile::unknown:
      return "unknown";
    case GroundTile::land:
      return "land";
    case GroundTile::water:
      return "water";
    case GroundTile::coast:
      return "coast";
    default:
      return "???";
    }
  }

  /**
   * Transform the given geo coordinate to a corresponding GroundTile coordinate
   *
   * @param point
   *    geo coordinate to transform
   * @param stateMap
   *    the StateMap, holding information about cell dimensions
   * @param cellMinLat
   *    offset of the ground map
   * @param cellMinLon
   *    offset of the ground map
   * @param coast
   *    Flag to be copied into the GroundTile coordinate
   * @return
   */
  GroundTile::Coord WaterIndexProcessor::Transform(const GeoCoord& point,
                                                   const StateMap& stateMap,
                                                   double cellMinLat,
                                                   double cellMinLon,
                                                   bool coast)
  {
    //std::cout << "       " << (coast?"*":"+") << " " << point.GetDisplayText() << std::endl;
    GroundTile::Coord coord(floor((point.GetLon()-cellMinLon)/stateMap.GetCellWidth()*GroundTile::Coord::CELL_MAX+0.5),
                            floor((point.GetLat()-cellMinLat)/stateMap.GetCellHeight()*GroundTile::Coord::CELL_MAX+0.5),
                            coast);

    return coord;
  }

  /**
   * Sets the size of the bitmap and initializes state of all tiles to "unknown"
   */
  void WaterIndexProcessor::Level::SetBox(const GeoBox& boundingBox,
                                          double cellWidth,
                                          double cellHeight)
  {
    indexEntryOffset=0;

    hasCellData=false;
    defaultCellData=State::unknown;

    indexDataOffset=0;

    stateMap.SetBox(boundingBox,
                   cellWidth,
                   cellHeight);
  }

  /**
   * Cut path `src` from point `start` (inclusive)
   * to `end` (exclusive) and store result to `dst`
   */
  static void CutPath(std::vector<Point>& dst,
                      const std::vector<Point>& src,
                      size_t start,
                      size_t end,
                      double startDistanceSquare,
                      double endDistanceSquare)
  {
    start=start%src.size();
    end=end%src.size();

    if (start>end || (start==end && startDistanceSquare>endDistanceSquare)) {
      dst.insert(dst.end(),
                 src.begin()+start,
                 src.end());
      dst.insert(dst.end(),
                 src.begin(),
                 src.begin()+end);
    }
    else {
      dst.insert(dst.end(),
                 src.begin()+start,
                 src.begin()+end);
    }
  }

  bool PathIntersectionSortA(const PathIntersection &i1, const PathIntersection &i2)
  {
    if (i1.aIndex==i2.aIndex)
      return i1.aDistanceSquare < i2.aDistanceSquare;
    return i1.aIndex < i2.aIndex;
  }

  bool PathIntersectionSortB(const PathIntersection &i1, const PathIntersection &i2)
  {
    if (i1.bIndex==i2.bIndex)
      return i1.bDistanceSquare < i2.bDistanceSquare;
    return i1.bIndex < i2.bIndex;
  }

  /**
   * Markes a cell as "coast", if one of the coastlines intersects with it.
   *
   */
  void WaterIndexProcessor::MarkCoastlineCells(Progress& progress,
                                               StateMap& stateMap,
                                               const Data& data)
  {
    progress.Info("Marking cells containing coastlines");

    for (const auto& coastline : data.coastlines) {
      // Marks cells on the path as coast
      std::set<Pixel> coords;

      GetCells(stateMap,
               coastline->points,
               coords);

      for (const auto& coord : coords) {
        if (stateMap.IsInAbsolute(coord.x,coord.y)) {
          if (stateMap.GetStateAbsolute(coord.x,coord.y)==unknown) {
#if defined(DEBUG_TILING)
            std::cout << "Coastline: " << coord.x-level.cellXStart << "," << coord.y-level.cellYStart << " " << coastline->id << std::endl;
#endif
            stateMap.SetStateAbsolute(coord.x,coord.y,coast);
          }
        }
      }

    }
  }

  /**
   * Calculate the cell type for cells directly around coast cells
   * @param progress
   * @param stateMap
   * @param cellGroundTileMap
   */
  void WaterIndexProcessor::CalculateCoastEnvironment(Progress& progress,
                                                      StateMap& stateMap,
                                                      const std::map<Pixel,std::list<GroundTile> >& cellGroundTileMap)
  {
    progress.Info("Calculate coast cell environment");

    for (const auto& tileEntry : cellGroundTileMap) {
      Pixel coord=tileEntry.first;
      State  state[4];      // type of the neighbouring cells: top, right, bottom, left
      size_t coordCount[4]; // number of coords on the given line (top, right, bottom, left)

      state[0]=unknown;
      state[1]=unknown;
      state[2]=unknown;
      state[3]=unknown;

      coordCount[0]=0;
      coordCount[1]=0;
      coordCount[2]=0;
      coordCount[3]=0;

      // Preset top
      if (coord.y<stateMap.GetYCount()-1) {
        state[0]=stateMap.GetState(coord.x,coord.y+1);
      }

      // Preset right
      if (coord.x<stateMap.GetXCount()-1) {
        state[1]=stateMap.GetState(coord.x+1,coord.y);
      }

      // Preset bottom
      if (coord.y>0) {
        state[2]=stateMap.GetState(coord.x,coord.y-1);
      }

      // Preset left
      if (coord.x>0) {
        state[3]=stateMap.GetState(coord.x-1,coord.y);
      }

      // Identify 'land' cells in relation to 'coast' cells
      for (const auto& tile : tileEntry.second) {
        State tileState=State::unknown;
        switch(tile.type){
          case GroundTile::unknown:
            tileState=State::unknown;
            break;
          case GroundTile::land:
            tileState=State::land;
            break;
          case GroundTile::water:
            tileState=State::water;
            break;
          case GroundTile::coast:
            tileState=State::unknown;
            break;
        }
        for (size_t c=0; c<tile.coords.size()-1;c++) {

          //
          // Count number of coords *on* the border
          //

          // top
          if (tile.coords[c].y==GroundTile::Coord::CELL_MAX) {
            coordCount[0]++;
          }
          // right
          else if (tile.coords[c].x==GroundTile::Coord::CELL_MAX) {
            coordCount[1]++;
          }
          // bottom
          else if (tile.coords[c].y==0) {
            coordCount[2]++;
          }
          // left
          else if (tile.coords[c].x==0) {
            coordCount[3]++;
          }

          //
          // Detect fills over a complete border
          //

          // line at the top from left to right => land is above current cell
          if (tile.coords[c].x==0 &&
              tile.coords[c].y==GroundTile::Coord::CELL_MAX &&
              tile.coords[c+1].x==GroundTile::Coord::CELL_MAX &&
              tile.coords[c+1].y==GroundTile::Coord::CELL_MAX) {
            if (state[0]==unknown) {
              state[0]=tileState;
            }
          }

          // Line from right top to bottom => land is right of current cell
          if (tile.coords[c].x==GroundTile::Coord::CELL_MAX &&
              tile.coords[c].y==GroundTile::Coord::CELL_MAX &&
              tile.coords[c+1].x==GroundTile::Coord::CELL_MAX &&
              tile.coords[c+1].y==0) {
            if (state[1]==unknown) {
              state[1]=tileState;
            }
          }

          // Line a the bottom from right to left => land is below current cell
          if (tile.coords[c].x==GroundTile::Coord::CELL_MAX &&
              tile.coords[c].y==0 &&
              tile.coords[c+1].x==0 &&
              tile.coords[c+1].y==0) {
            if (state[2]==unknown) {
              state[2]=tileState;
            }
          }

          // Line left from bottom to top => land is left of current cell
          if (tile.coords[c].x==0 &&
              tile.coords[c].y==0 &&
              tile.coords[c+1].x==0 &&
              tile.coords[c+1].y==GroundTile::Coord::CELL_MAX) {
            if (state[3]==unknown) {
              state[3]=tileState;
            }
          }
        }
      }

      // top
      if (coord.y<stateMap.GetYCount()-1 &&
        stateMap.GetState(coord.x,coord.y+1)==unknown) {
        if (state[0]!=unknown) {
#if defined(DEBUG_TILING)
          std::cout << "Assume " << StateToString(state[0]) << " above coast: " << coord.x << "," << coord.y+1 << std::endl;
#endif
          stateMap.SetState(coord.x,coord.y+1,state[0]);
        }
      }

      if (coord.x<stateMap.GetXCount()-1 &&
        stateMap.GetState(coord.x+1,coord.y)==unknown) {
        if (state[1]!=unknown) {
#if defined(DEBUG_TILING)
          std::cout << "Assume " << StateToString(state[1]) << " right of coast: " << coord.x+1 << "," << coord.y << std::endl;
#endif
          stateMap.SetState(coord.x+1,coord.y,state[1]);
        }
      }

      if (coord.y>0 &&
        stateMap.GetState(coord.x,coord.y-1)==unknown) {
        if (state[2]!=unknown) {
#if defined(DEBUG_TILING)
          std::cout << "Assume " << StateToString(state[2]) << " below coast: " << coord.x << "," << coord.y-1 << std::endl;
#endif
          stateMap.SetState(coord.x,coord.y-1,state[2]);
        }
      }

      if (coord.x>0 &&
        stateMap.GetState(coord.x-1,coord.y)==unknown) {
        if (state[3]!=unknown) {
#if defined(DEBUG_TILING)
          std::cout << "Assume " << StateToString(state[3]) << " left of coast: " << coord.x-1 << "," << coord.y << std::endl;
#endif
          stateMap.SetState(coord.x-1,coord.y,state[3]);
        }
      }
    }
  }

  bool WaterIndexProcessor::IsCellInBoundingPolygon(const CellBoundaries& cellBoundary,
                                                    const std::list<CoastRef>& boundingPolygons)
  {
    if (boundingPolygons.empty()){
      return true;
    }

    std::vector<GeoCoord> cellCoords;
    cellCoords.assign(cellBoundary.borderPoints, cellBoundary.borderPoints + 4);
    for (const auto &poly:boundingPolygons){
      if (IsAreaAtLeastPartlyInArea(cellCoords,poly->coast)){
        return true;
      }
    }
    return false;
  }

  /**
   * Marks all still 'unknown' cells neighbouring 'water' cells as 'water', too
   *
   * Converts all cells of state "unknown" that touch a tile with state
   * "water" to state "water", too.
   */
  void WaterIndexProcessor::FillWater(Progress& progress,
                                      Level& level,
                                      size_t tileCount,
                                      const std::list<CoastRef>& boundingPolygons)
  {
    progress.Info("Filling water");

    for (size_t i=1; i<=tileCount; i++) {
      Level newLevel(level);

      for (uint32_t y=0; y<level.stateMap.GetYCount(); y++) {
        for (uint32_t x=0; x<level.stateMap.GetXCount(); x++) {
          if (level.stateMap.GetState(x,y)==water) {

            // avoid filling of water outside data polygon
            if (!IsCellInBoundingPolygon(CellBoundaries(level.stateMap,
                                                        Pixel(x,y)),
                                         boundingPolygons)){
              continue;
            }

            if (y>0) {
              if (level.stateMap.GetState(x,y-1)==unknown) {
#if defined(DEBUG_TILING)
                std::cout << "Water below water: " << x << "," << y-1 << std::endl;
#endif
                newLevel.stateMap.SetState(x,y-1,water);
              }
            }

            if (y<level.stateMap.GetYCount()-1) {
              if (level.stateMap.GetState(x,y+1)==unknown) {
#if defined(DEBUG_TILING)
                std::cout << "Water above water: " << x << "," << y+1 << std::endl;
#endif
                newLevel.stateMap.SetState(x,y+1,water);
              }
            }

            if (x>0) {
              if (level.stateMap.GetState(x-1,y)==unknown) {
#if defined(DEBUG_TILING)
                std::cout << "Water left of water: " << x-1 << "," << y << std::endl;
#endif
                newLevel.stateMap.SetState(x-1,y,water);
              }
            }

            if (x<level.stateMap.GetXCount()-1) {
              if (level.stateMap.GetState(x+1,y)==unknown) {
#if defined(DEBUG_TILING)
                std::cout << "Water right of water: " << x+1 << "," << y << std::endl;
#endif
                newLevel.stateMap.SetState(x+1,y,water);
              }
            }
          }
        }
      }

      level=newLevel;
    }
  }

  bool WaterIndexProcessor::ContainsCoord(const std::list<GroundTile>& tiles,
                                          const GroundTile::Coord& coord,
                                          GroundTile::Type type)
  {
    for (const auto& tile : tiles) {
      if (tile.type==type) {
        for (const auto& c : tile.coords) {
          if (c==coord) {
            return true;
          }
        }
      }
    }
    return false;
  }

  bool WaterIndexProcessor::ContainsCoord(const std::list<GroundTile>& tiles,
                                          const GroundTile::Coord& coord)
  {
    for (const auto& tile : tiles) {
      for (const auto& c : tile.coords) {
        if (c==coord) {
          return true;
        }
      }
    }
    return false;
  }

  bool WaterIndexProcessor::ContainsWater(const Pixel& coord,
                                          const StateMap& stateMap,
                                          const std::map<Pixel,std::list<GroundTile>>& cellGroundTileMap,
                                          const GroundTile::Coord& testCoord1,
                                          const GroundTile::Coord& testCoord2)
  {
    if (coord.x>=stateMap.GetXCount() ||
        coord.y>=stateMap.GetYCount()) {
      return false;
    }

    if (stateMap.GetState(coord.x,coord.y)==water){
      return true;
    }

    const auto &tilesEntry=cellGroundTileMap.find(coord);
    if (tilesEntry==cellGroundTileMap.end()){
      return false;
    }

    return ContainsCoord(tilesEntry->second,testCoord1,GroundTile::water) ||
           ContainsCoord(tilesEntry->second,testCoord2,GroundTile::water);
  }

  void WaterIndexProcessor::FillWaterAroundIsland(Progress& progress,
                                                  StateMap& stateMap,
                                                  std::map<Pixel,std::list<GroundTile>>& cellGroundTileMap,
                                                  const std::list<CoastRef>& boundingPolygons)
  {
    progress.Info("Filling water around islands");

    for (const auto &entry:cellGroundTileMap){
      Pixel coord=entry.first;
      CellBoundaries cellBoundaries(stateMap,coord);

      if (ContainsCoord(entry.second, cellBoundaries.borderCoords[0]) ||
          ContainsCoord(entry.second, cellBoundaries.borderCoords[1]) ||
          ContainsCoord(entry.second, cellBoundaries.borderCoords[2]) ||
          ContainsCoord(entry.second, cellBoundaries.borderCoords[3])){
        continue;
      }
      // cell with some GroundTile, but all borderCoors are missing
      // => it contains island(s)

      // avoid filling of water outside data polygon
      if (!IsCellInBoundingPolygon(CellBoundaries(stateMap,coord),
                                   boundingPolygons)){
        continue;
      }

      bool fillWater=false;

      // test if some tiles around contains water

      // top
      if (!fillWater && coord.y>0 && ContainsWater(Pixel(coord.x,coord.y-1),
                                                   stateMap,
                                                   cellGroundTileMap,
                                                   cellBoundaries.borderCoords[0],
                                                   cellBoundaries.borderCoords[1])){
        fillWater=true;
      }
      // bottom
      if (!fillWater && ContainsWater(Pixel(coord.x, coord.y+1),
                                      stateMap,
                                      cellGroundTileMap,
                                      cellBoundaries.borderCoords[2],
                                      cellBoundaries.borderCoords[3])){
        fillWater=true;
      }
      // left
      if (!fillWater && coord.x>0 && ContainsWater(Pixel(coord.x-1, coord.y),
                                                   stateMap,
                                                   cellGroundTileMap,
                                                   cellBoundaries.borderCoords[0],
                                                   cellBoundaries.borderCoords[3])){
        fillWater=true;
      }
      // right
      if (!fillWater && ContainsWater(Pixel(coord.x+1, coord.y),
                                      stateMap,
                                      cellGroundTileMap,
                                      cellBoundaries.borderCoords[1],
                                      cellBoundaries.borderCoords[2])){
        fillWater=true;
      }

      if (fillWater){
        GroundTile groundTile(GroundTile::water);
#if defined(DEBUG_TILING)
        std::cout << "Add water base to tile with islands: " << coord.x << "," << coord.y << std::endl;
#endif

        groundTile.coords.push_back(cellBoundaries.borderCoords[0]);
        groundTile.coords.push_back(cellBoundaries.borderCoords[1]);
        groundTile.coords.push_back(cellBoundaries.borderCoords[2]);
        groundTile.coords.push_back(cellBoundaries.borderCoords[3]);

        // water GroundTile as "top layer"
        cellGroundTileMap[coord].push_front(groundTile);
      }
    }
  }

  /**
   * Marks all still 'unknown' cells between 'coast' or 'land' and 'land' cells as 'land', too
   *
   * Scanning from left to right and bottom to top: Every tile that is unknown
   * but is placed between land and coast or land cells must be land, too.
   */
  void WaterIndexProcessor::FillLand(Progress& progress,
                                     StateMap& stateMap)
  {
    progress.Info("Filling land");

    bool cont=true;

    while (cont) {
      cont=false;

      // Left to right
      for (uint32_t y=0; y<stateMap.GetYCount(); y++) {
        uint32_t x=0;
        uint32_t start=0;
        uint32_t end=0;
        uint32_t state=0;

        while (x<stateMap.GetXCount()) {
          switch (state) {
            case 0:
              if (stateMap.GetState(x,y)==land) {
                state=1;
              }
              x++;
              break;
            case 1:
              if (stateMap.GetState(x,y)==unknown) {
                state=2;
                start=x;
                end=x;
                x++;
              }
              else {
                state=0;
              }
              break;
            case 2:
              if (stateMap.GetState(x,y)==unknown) {
                end=x;
                x++;
              }
              else if (stateMap.GetState(x,y)==coast || stateMap.GetState(x,y)==land) {
                if (start<stateMap.GetXCount() && end<stateMap.GetXCount() && start<=end) {
                  for (uint32_t i=start; i<=end; i++) {
#if defined(DEBUG_TILING)
                    std::cout << "Land between: " << i << "," << y << std::endl;
#endif
                    stateMap.SetState(i,y,land);
                    cont=true;
                  }
                }

                state=0;
              }
              else {
                state=0;
              }
              break;
          }
        }
      }

      //Bottom Up
      for (uint32_t x=0; x<stateMap.GetXCount(); x++) {
        uint32_t y=0;
        uint32_t start=0;
        uint32_t end=0;
        uint32_t state=0;

        while (y<stateMap.GetYCount()) {
          switch (state) {
            case 0:
              if (stateMap.GetState(x,y)==land) {
                state=1;
              }
              y++;
              break;
            case 1:
              if (stateMap.GetState(x,y)==unknown) {
                state=2;
                start=y;
                end=y;
                y++;
              }
              else {
                state=0;
              }
              break;
            case 2:
              if (stateMap.GetState(x,y)==unknown) {
                end=y;
                y++;
              }
              else if (stateMap.GetState(x,y)==coast || stateMap.GetState(x,y)==land) {
                if (start<stateMap.GetYCount() && end<stateMap.GetYCount() && start<=end) {
                  for (uint32_t i=start; i<=end; i++) {
#if defined(DEBUG_TILING)
                    std::cout << "Land between: " << x << "," << i << std::endl;
#endif
                    stateMap.SetState(x,i,land);
                    cont=true;
                  }
                }

                state=0;
              }
              else {
                state=0;
              }
              break;
          }
        }
      }
    }
  }

  void WaterIndexProcessor::DumpIndexHeader(FileWriter& writer,
                                            std::vector<Level>& levels)
  {
    writer.WriteNumber((uint32_t)(levels.front().level));
    writer.WriteNumber((uint32_t)(levels.back().level));

    for (auto& level : levels) {
      level.indexEntryOffset=writer.GetPos();
      writer.Write(level.hasCellData);
      writer.Write(level.dataOffsetBytes);
      writer.Write((uint8_t)level.defaultCellData);
      writer.WriteFileOffset(level.indexDataOffset);
      writer.WriteNumber(level.stateMap.GetXStart());
      writer.WriteNumber(level.stateMap.GetXEnd());
      writer.WriteNumber(level.stateMap.GetYStart());
      writer.WriteNumber(level.stateMap.GetYEnd());
    }
  }

  /**
   * Fills coords information for cells that completely contain a coastline
   *
   * @param progress
   * @param level
   * @param data
   * @param cellGroundTileMap
   */
  void WaterIndexProcessor::HandleAreaCoastlinesCompletelyInACell(Progress& progress,
                                                                  const StateMap& stateMap,
                                                                  Data& data,
                                                                  std::map<Pixel,std::list<GroundTile> >& cellGroundTileMap)
  {
    progress.Info("Handle area coastline completely in a cell");

    size_t currentCoastline=1;
    for (const auto& coastline : data.coastlines) {
      progress.SetProgress(currentCoastline,data.coastlines.size());

      currentCoastline++;

      if (!(coastline->isArea &&
            coastline->isCompletelyInCell)) {
        continue;
      }

      if (!stateMap.IsInAbsolute(coastline->cell.x,coastline->cell.y)) {
        continue;
      }

      Pixel            coord(coastline->cell.x-stateMap.GetXStart(),
                             coastline->cell.y-stateMap.GetYStart());
      GroundTile::Type type=GroundTile::land;

      if (coastline->left==CoastState::unknown) {
        type=GroundTile::unknown;
      }
      else if (coastline->left==CoastState::water) {
        type=GroundTile::water; // should not happen on the Earth
      }

      GroundTile groundTile(type);

      double cellMinLat=stateMap.GetCellHeight()*coastline->cell.y-90.0;
      double cellMinLon=stateMap.GetCellWidth()*coastline->cell.x-180.0;

      groundTile.coords.reserve(coastline->points.size());

      for (size_t p=0; p<coastline->points.size(); p++) {
        groundTile.coords.push_back(Transform(coastline->points[p],
                                              stateMap,
                                              cellMinLat,cellMinLon,
                                              true));
      }

      if (!groundTile.coords.empty()) {
        groundTile.coords.back().coast=false;

#if defined(DEBUG_TILING)
        std::cout << "Coastline in cell: " << coord.x << "," << coord.y << std::endl;
#endif

        cellGroundTileMap[coord].push_back(groundTile);
      }
    }
  }

  static bool IsLeftOnSameBorder(size_t border, const GeoCoord& a,const GeoCoord& b)
  {
    switch (border) {
    case 0:
      return b.GetLon()>=a.GetLon();
    case 1:
      return b.GetLat()<=a.GetLat();
    case 2:
      return b.GetLon()<=a.GetLon();
    case 3:
      return b.GetLat()>=a.GetLat();
    }

    assert(false);

    return false;
  }

  /**
   * Return the list of StateMap cells a line between coordinate a and coordinate b
   * intersects with.
   *
   * @param stateMap
   *    StateMap defining the cell structure
   * @param a
   *    Start of the line
   * @param b
   *    End of the line
   * @param cellIntersections
   *    List of cells, the line intersects with
   */
  void WaterIndexProcessor::GetCells(const StateMap& stateMap,
                                     const GeoCoord& a,
                                     const GeoCoord& b,
                                     std::set<Pixel>& cellIntersections) const
  {
    uint32_t cx1=(uint32_t)((a.GetLon()+180.0)/stateMap.GetCellWidth());
    uint32_t cy1=(uint32_t)((a.GetLat()+90.0)/stateMap.GetCellHeight());

    uint32_t cx2=(uint32_t)((b.GetLon()+180.0)/stateMap.GetCellWidth());
    uint32_t cy2=(uint32_t)((b.GetLat()+90.0)/stateMap.GetCellHeight());

    cellIntersections.insert(Pixel(cx1,cy1));

    if (cx1!=cx2 || cy1!=cy2) {
      for (uint32_t x=std::min(cx1,cx2); x<=std::max(cx1,cx2); x++) {
        for (uint32_t y=std::min(cy1,cy2); y<=std::max(cy1,cy2); y++) {

          Pixel    coord(x,y);
          GeoCoord borderPoints[5];
          double   lonMin,lonMax,latMin,latMax;

          lonMin=x*stateMap.GetCellWidth()-180.0;
          lonMax=lonMin+stateMap.GetCellWidth();
          latMin=y*stateMap.GetCellHeight()-90.0;
          latMax=latMin+stateMap.GetCellHeight();

          borderPoints[0].Set(latMax,lonMin); // top left
          borderPoints[1].Set(latMax,lonMax); // top right
          borderPoints[2].Set(latMin,lonMax); // bottom right
          borderPoints[3].Set(latMin,lonMin); // bottom left
          borderPoints[4]=borderPoints[0];    // To avoid "% 4" on all indexes

          size_t corner=0;

          while (corner<4) {
            if (LinesIntersect(a,
                               b,
                               borderPoints[corner],
                               borderPoints[corner+1])) {
              cellIntersections.insert(coord);

              break;
            }

            corner++;
          }
        }
      }
    }
  }

  /**
   * Return the list of StateMap cells the given path intersects with
   *
   * @param stateMap
   *    StateMap defining the cell structure
   * @param points
   *    Path
   * @param cellIntersections
   *    List of cells, the path intersects with
   */
  void WaterIndexProcessor::GetCells(const StateMap& stateMap,
                                     const std::vector<GeoCoord>& points,
                                     std::set<Pixel>& cellIntersections) const
  {
    for (size_t p=0; p<points.size()-1; p++) {
      GetCells(stateMap,points[p],points[p+1],cellIntersections);
    }
  }

  /**
   * Return the list of StateMap cells the given path intersects with
   *
   * @param stateMap
   *    StateMap defining the cell structure
   * @param points
   *    Path
   * @param cellIntersections
   *    List of cells, the path intersects with
   */
  void WaterIndexProcessor::GetCells(const StateMap& stateMap,
                                     const std::vector<Point>& points,
                                     std::set<Pixel>& cellIntersections) const
  {
    for (size_t p=0; p<points.size()-1; p++) {
      GetCells(stateMap,points[p].GetCoord(),points[p+1].GetCoord(),cellIntersections);
    }
  }

  void WaterIndexProcessor::GetCellIntersections(const StateMap& stateMap,
                                                 const std::vector<GeoCoord>& points,
                                                 size_t coastline,
                                                 std::map<Pixel,std::list<IntersectionRef>>& cellIntersections)
  {
    for (size_t p=0; p<points.size()-1; p++) {
      // Cell coordinates of the current and the next point
      uint32_t cx1=(uint32_t)((points[p].GetLon()+180.0)/stateMap.GetCellWidth());
      uint32_t cy1=(uint32_t)((points[p].GetLat()+90.0)/stateMap.GetCellHeight());

      uint32_t cx2=(uint32_t)((points[p+1].GetLon()+180.0)/stateMap.GetCellWidth());
      uint32_t cy2=(uint32_t)((points[p+1].GetLat()+90.0)/stateMap.GetCellHeight());

      if (cx1!=cx2 || cy1!=cy2) {
        for (uint32_t x=std::min(cx1,cx2); x<=std::max(cx1,cx2); x++) {
          for (uint32_t y=std::min(cy1,cy2); y<=std::max(cy1,cy2); y++) {

            if (!stateMap.IsInAbsolute(x,y)) {
              continue;
            }

            Pixel    coord(x-stateMap.GetXStart(),
                           y-stateMap.GetYStart());
            GeoCoord borderPoints[5];
            double   lonMin,lonMax,latMin,latMax;

            lonMin=x*stateMap.GetCellWidth()-180.0;
            lonMax=(x+1)*stateMap.GetCellWidth()-180.0;
            latMin=y*stateMap.GetCellHeight()-90.0;
            latMax=(y+1)*stateMap.GetCellHeight()-90.0;

            borderPoints[0].Set(latMax,lonMin); // top left
            borderPoints[1].Set(latMax,lonMax); // top right
            borderPoints[2].Set(latMin,lonMax); // bottom right
            borderPoints[3].Set(latMin,lonMin); // bottom left
            borderPoints[4]=borderPoints[0];    // To avoid modula 4 on all indexes

            size_t          intersectionCount=0;
            IntersectionRef firstIntersection=std::make_shared<Intersection>();
            IntersectionRef secondIntersection=std::make_shared<Intersection>();
            size_t          corner=0;

            // Check intersection with one of the borders
            while (corner<4) {
              if (GetLineIntersection(points[p],
                                      points[p+1],
                                      borderPoints[corner],
                                      borderPoints[corner+1],
                                      firstIntersection->point)) {
                intersectionCount++;

                firstIntersection->coastline=coastline;
                firstIntersection->prevWayPointIndex=p;
                firstIntersection->distanceSquare=DistanceSquare(points[p],firstIntersection->point);
                firstIntersection->borderIndex=corner;

                corner++;
                break;
              }

              corner++;
            }

            // Check if there is another intersection with one of the following borders
            while (corner<4) {
              if (GetLineIntersection(points[p],
                                      points[p+1],
                                      borderPoints[corner],
                                      borderPoints[corner+1],
                                      secondIntersection->point)) {
                intersectionCount++;

                secondIntersection->coastline=coastline;
                secondIntersection->prevWayPointIndex=p;
                secondIntersection->distanceSquare=DistanceSquare(points[p],secondIntersection->point);
                secondIntersection->borderIndex=corner;

                corner++;
                break;
              }

              corner++;
            }

            // After above steps we can have 0..2 intersections

            if (x==cx1 &&
                y==cy1) {
              assert(intersectionCount==1 ||
                     intersectionCount==2);

              if (intersectionCount==1) {
                // The segment always leaves the origin cell
                firstIntersection->direction=Direction::out;
                cellIntersections[coord].push_back(firstIntersection);
              }
              else if (intersectionCount==2) {
                // If we have two intersections with borders of cells between the starting cell and the
                // target cell then the one closer to the starting point is the incoming and the one farer
                // away is the leaving one
                double firstLength=DistanceSquare(firstIntersection->point,points[p]);
                double secondLength=DistanceSquare(secondIntersection->point,points[p]);

                if (firstLength<=secondLength) {
                  firstIntersection->direction=Direction::in; // Enter
                  cellIntersections[coord].push_back(firstIntersection);

                  secondIntersection->direction=Direction::out; // Leave
                  cellIntersections[coord].push_back(secondIntersection);
                }
                else {
                  secondIntersection->direction=Direction::in; // Enter
                  cellIntersections[coord].push_back(secondIntersection);

                  firstIntersection->direction=Direction::out; // Leave
                  cellIntersections[coord].push_back(firstIntersection);

                }
              }
            }
            else if (x==cx2 &&
                     y==cy2) {
              assert(intersectionCount==1 ||
                     intersectionCount==2);

              if (intersectionCount==1) {
                // The segment always enters the target cell
                firstIntersection->direction=Direction::in;
                cellIntersections[coord].push_back(firstIntersection);
              }
              else if (intersectionCount==2) {
                // If we have two intersections with borders of cells between the starting cell and the
                // target cell then the one closer to the starting point is the incoming and the one farer
                // away is the leaving one
                double firstLength=DistanceSquare(firstIntersection->point,points[p]);
                double secondLength=DistanceSquare(secondIntersection->point,points[p]);

                if (firstLength<=secondLength) {
                  firstIntersection->direction=Direction::in; // Enter
                  cellIntersections[coord].push_back(firstIntersection);

                  secondIntersection->direction=Direction::out; // Leave
                  cellIntersections[coord].push_back(secondIntersection);
                }
                else {
                  secondIntersection->direction=Direction::in; // Enter
                  cellIntersections[coord].push_back(secondIntersection);

                  firstIntersection->direction=Direction::out; // Leave
                  cellIntersections[coord].push_back(firstIntersection);

                }
              }
            }
            else {
              assert(intersectionCount==0 ||
                     intersectionCount==1 ||
                     intersectionCount==2);

              if (intersectionCount==1) {
                // If we only have one intersection with borders of cells between the starting borderPoints and the
                // target borderPoints then this is a "touch"
                firstIntersection->direction=Direction::touch;
                cellIntersections[coord].push_back(firstIntersection);
              }
              else if (intersectionCount==2) {
                // If we have two intersections with borders of cells between the starting cell and the
                // target cell then the one closer to the starting point is the incoming and the one farer
                // away is the leaving one
                double firstLength=DistanceSquare(firstIntersection->point,points[p]);
                double secondLength=DistanceSquare(secondIntersection->point,points[p]);

                if (firstLength<=secondLength) {
                  firstIntersection->direction=Direction::in; // Enter
                  cellIntersections[coord].push_back(firstIntersection);

                  secondIntersection->direction=Direction::out; // Leave
                  cellIntersections[coord].push_back(secondIntersection);
                }
                else {
                  secondIntersection->direction=Direction::in; // Enter
                  cellIntersections[coord].push_back(secondIntersection);

                  firstIntersection->direction=Direction::out; // Leave
                  cellIntersections[coord].push_back(firstIntersection);

                }
              }
            }
          }
        }
      }
    }
  }

  /**
   * Collects, calculates and generates a number of data about a coastline.
   */
  void WaterIndexProcessor::CalculateCoastlineData(Progress& progress,
                                                   TransPolygon::OptimizeMethod optimizationMethod,
                                                   double tolerance,
                                                   double minObjectDimension,
                                                   const Projection& projection,
                                                   const StateMap& stateMap,
                                                   const std::list<CoastRef>& coastlines,
                                                   Data& data)
  {
    progress.Info("Calculate coastline data");

    int                           index=-1;
    std::vector<CoastlineDataRef> transformedCoastlines;
    std::vector<CoastRef>         coasts;

    transformedCoastlines.resize(coastlines.size());
    coasts.resize(coastlines.size());

    for (const auto& coast : coastlines) {
      index++;
      progress.SetProgress(index,coastlines.size());

      TransPolygon polygon;

      // For areas we first transform the bounding box to make sure, that
      // the area coastline will be big enough to be actually visible
      if (coast->isArea) {
        GeoBox boundingBox;

        GetBoundingBox(coast->coast,
                       boundingBox);

        polygon.TransformBoundingBox(projection,
                                     optimizationMethod,
                                     boundingBox,
                                     1.0,
                                     TransPolygon::simple);

        double minX=polygon.points[polygon.GetStart()].x;
        double minY=polygon.points[polygon.GetStart()].y;
        double maxX=minX;
        double maxY=minY;

        for (size_t p=polygon.GetStart()+1; p<=polygon.GetEnd(); p++) {
          if (polygon.points[p].draw) {
            minX=std::min(minX,polygon.points[p].x);
            maxX=std::max(maxX,polygon.points[p].x);
            minY=std::min(minY,polygon.points[p].y);
            maxY=std::max(maxY,polygon.points[p].y);
          }
        }

        double pixelWidth=maxX-minX;
        double pixelHeight=maxY-minY;

        // Artificial values but for drawing an area a box of at least 4x4 might make sense
        if (pixelWidth<=minObjectDimension ||
            pixelHeight<=minObjectDimension) {
          continue;
        }
      }

      if (coast->isArea) {
        polygon.TransformArea(projection,
                              optimizationMethod,
                              coast->coast,
                              tolerance,
                              TransPolygon::simple);
      }
      else {
        polygon.TransformWay(projection,
                             optimizationMethod,
                             coast->coast,
                             tolerance,
                             TransPolygon::simple);
      }

      CoastlineDataRef coastline=std::make_shared<CoastlineData>();

      coastline->id=coast->id;
      coastline->isArea=coast->isArea;
      coastline->right=coast->right;
      coastline->left=coast->left;
      coastline->points.reserve(polygon.GetLength());

      for (size_t p=polygon.GetStart(); p<=polygon.GetEnd(); p++) {
        if (polygon.points[p].draw) {
          coastline->points.push_back(coast->coast[p].GetCoord());
        }
      }

      // Currently transformation optimization code sometimes does not correctly handle the closing point for areas
      if (coast->isArea) {
        if (coastline->points.front()!=coastline->points.back()) {
          coastline->points.push_back(coastline->points.front());
        }

        if (coastline->points.size()<=3) {
          // ignore island reduced just to line
          continue;
        }
      }

      transformedCoastlines[index]=coastline;
      coasts[index]=coast;
    }

    /* In some countries are islands too close to land or other islands
     * that its coastlines intersect after polygon optimisation.
     *
     * It may cause problems in following computation, that strongly rely
     * on the fact that coastlines don't intersect.
     *
     * For that reason, we will try to detect such intersections between land
     * (line coastlines) and islands (area coastlines) to remove the most visible
     * errors. Detecting intersections between all islands is too expensive.
     */

    bool haveAreas=false;
    bool haveWays=false;

    for (size_t i=0; i<transformedCoastlines.size(); i++) {
      if (!transformedCoastlines[i]) {
        continue;
      }

      if (transformedCoastlines[i]->isArea) {
        haveAreas=true;
      }

      if (!transformedCoastlines[i]->isArea) {
        haveWays=true;
      }

      if (haveAreas && haveWays) {
        break;
      }
    }

    if (haveAreas && haveWays) {
      progress.Info("Filter intersecting islands");
      
      for (size_t i=0; i<transformedCoastlines.size(); i++) {
        progress.SetProgress(i,transformedCoastlines.size());

        for (size_t j=i+1; j<transformedCoastlines.size(); j++) {
          CoastlineDataRef a=transformedCoastlines[i];
          CoastlineDataRef b=transformedCoastlines[j];

          if (!a || !b || (a->isArea == b->isArea)) {
            // ignore possible intersections between two coastline ways (it may be touching)
            // or two coastline areas (it is not so problematic and its computation is expensive)
            continue;
          }

          std::vector<PathIntersection> intersections;

          FindPathIntersections(a->points,
                                b->points,
                                a->isArea,
                                b->isArea,
                                intersections);

          if (!intersections.empty()) {
            progress.Warning("Detected intersection "+NumberToString(coasts[i]->id)+" <> "+NumberToString(coasts[j]->id));

            if (a->isArea && !b->isArea) {
              transformedCoastlines[i]=NULL;
              coasts[i]=NULL;
            }
            else if (b->isArea && !a->isArea) {
              transformedCoastlines[j]=NULL;
              coasts[j]=NULL;
            }
          }
        }
      }
    }

    progress.Info("Calculate covered tiles");
    size_t curCoast=0;

    data.coastlines.resize(transformedCoastlines.size());

    for (size_t index=0; index<transformedCoastlines.size(); index++) {
      progress.SetProgress(index,transformedCoastlines.size());

      CoastlineDataRef coastline=transformedCoastlines[index];

      if (!coastline) {
        continue;
      }

      CoastRef coast=coasts[index];

      data.coastlines[curCoast]=coastline;

      GeoBox boundingBox;

      GetBoundingBox(coast->coast,
                     boundingBox);

      uint32_t cxMin,cxMax,cyMin,cyMax;

      cxMin=(uint32_t)floor((boundingBox.GetMinLon()+180.0)/stateMap.GetCellWidth());
      cxMax=(uint32_t)floor((boundingBox.GetMaxLon()+180.0)/stateMap.GetCellWidth());
      cyMin=(uint32_t)floor((boundingBox.GetMinLat()+90.0)/stateMap.GetCellHeight());
      cyMax=(uint32_t)floor((boundingBox.GetMaxLat()+90.0)/stateMap.GetCellHeight());

      if (cxMin==cxMax &&
          cyMin==cyMax) {
        coastline->cell.x=cxMin;
        coastline->cell.y=cyMin;
        coastline->isCompletelyInCell=true;

        if (stateMap.IsInAbsolute(coastline->cell.x,coastline->cell.y)) {
          Pixel coord(coastline->cell.x-stateMap.GetXStart(),
                      coastline->cell.y-stateMap.GetYStart());
          data.cellCoveredCoastlines[coord].push_back(curCoast);
        }
      }
      else {
        coastline->isCompletelyInCell=false;

        // Calculate all intersections for all path steps for all cells covered
        GetCellIntersections(stateMap,
                             coastline->points,
                             curCoast,
                             coastline->cellIntersections);

        for (const auto& intersectionEntry : coastline->cellIntersections) {
          data.cellCoastlines[intersectionEntry.first].push_back(curCoast);
        }
      }

      curCoast++;
    }

    // Fix the vector size to remove unused slots (because of filtering by min area size)
    data.coastlines.resize(curCoast);

    progress.Info("Initial "+NumberToString(coastlines.size())+" coastline(s) transformed to "+NumberToString(data.coastlines.size())+" coastline(s)");
  }

  /**
   * Closes the sling from the incoming intersection to the outgoing intersection traveling clock
   * wise around the cell border.
   */
  void WaterIndexProcessor::WalkBorderCW(GroundTile& groundTile,
                                         const StateMap& stateMap,
                                         double cellMinLat,
                                         double cellMinLon,
                                         const IntersectionRef& incoming,
                                         const IntersectionRef& outgoing,
                                         const GroundTile::Coord borderCoords[])
  {

    if (outgoing->borderIndex!=incoming->borderIndex ||
        !IsLeftOnSameBorder(incoming->borderIndex,incoming->point,outgoing->point)) {
      size_t borderPoint=(incoming->borderIndex+1)%4;
      size_t endBorderPoint=outgoing->borderIndex;

      while (borderPoint!=endBorderPoint) {
        groundTile.coords.push_back(borderCoords[borderPoint]);

        if (borderPoint==3) {
          borderPoint=0;
        }
        else {
          borderPoint++;
        }
      }

      groundTile.coords.push_back(borderCoords[borderPoint]);
    }

    groundTile.coords.push_back(Transform(outgoing->point,
                                          stateMap,
                                          cellMinLat,cellMinLon,
                                          false));
  }


  WaterIndexProcessor::IntersectionRef WaterIndexProcessor::GetNextCW(const std::list<IntersectionRef>& intersectionsCW,
                                                                      const IntersectionRef& current) const
  {
    std::list<IntersectionRef>::const_iterator next=intersectionsCW.begin();

    while (next!=intersectionsCW.end() &&
           (*next)!=current) {
      next++;
    }

    assert(next!=intersectionsCW.end());

    next++;

    if (next==intersectionsCW.end()) {
      next=intersectionsCW.begin();
    }

    assert(next!=intersectionsCW.end());

    return *next;
  }

  void WaterIndexProcessor::WalkPathBack(GroundTile& groundTile,
                                         const StateMap& stateMap,
                                         double cellMinLat,
                                         double cellMinLon,
                                         const IntersectionRef& pathStart,
                                         const IntersectionRef& pathEnd,
                                         const std::vector<GeoCoord>& points,
                                         bool isArea)
  {
    groundTile.coords.back().coast=true;

    if (isArea) {
      if (pathStart->prevWayPointIndex==pathEnd->prevWayPointIndex &&
          pathStart->distanceSquare>pathEnd->distanceSquare) {
        groundTile.coords.push_back(Transform(pathEnd->point,
                                              stateMap,
                                              cellMinLat,cellMinLon,
                                              false));
      }
      else {
        size_t idx=pathStart->prevWayPointIndex;
        size_t targetIdx=pathEnd->prevWayPointIndex+1;

        if (targetIdx==points.size()) {
          targetIdx=0;
        }

        while (idx!=targetIdx) {
          groundTile.coords.push_back(Transform(points[idx],
                                                stateMap,
                                                cellMinLat,cellMinLon,
                                                true));

          if (idx>0) {
            idx--;
          }
          else {
            idx=points.size()-1;
          }
        }

        groundTile.coords.push_back(Transform(points[idx],
                                              stateMap,
                                              cellMinLat,cellMinLon,
                                              true));

        groundTile.coords.push_back(Transform(pathEnd->point,
                                              stateMap,
                                              cellMinLat,cellMinLon,
                                              false));
      }
    }
    else {
      size_t targetIdx=pathEnd->prevWayPointIndex+1;

      for (size_t idx=pathStart->prevWayPointIndex;
          idx>=targetIdx;
          idx--) {
        groundTile.coords.push_back(Transform(points[idx],
                                              stateMap,
                                              cellMinLat,cellMinLon,
                                              true));
      }

      groundTile.coords.push_back(Transform(pathEnd->point,
                                            stateMap,
                                            cellMinLat,cellMinLon,
                                            false));
    }
  }

  void WaterIndexProcessor::WalkPathForward(GroundTile& groundTile,
                                            const StateMap& stateMap,
                                            double cellMinLat,
                                            double cellMinLon,
                                            const IntersectionRef& pathStart,
                                            const IntersectionRef& pathEnd,
                                            const std::vector<GeoCoord>& points,
                                            bool isArea)
  {
    groundTile.coords.back().coast=true;

    if (isArea) {
      if (pathStart->prevWayPointIndex==pathEnd->prevWayPointIndex &&
          pathStart->distanceSquare<pathEnd->distanceSquare) {
        groundTile.coords.push_back(Transform(pathEnd->point,
                                              stateMap,
                                              cellMinLat,cellMinLon,
                                              false));
      }
      else {
        size_t idx=pathStart->prevWayPointIndex+1;
        size_t targetIdx=pathEnd->prevWayPointIndex;

        if (targetIdx==points.size()) {
          targetIdx=0;
        }

        while (idx!=targetIdx) {
          groundTile.coords.push_back(Transform(points[idx],
                                                stateMap,
                                                cellMinLat,cellMinLon,
                                                true));

          if (idx>=points.size()-1) {
            idx=0;
          }
          else {
            idx++;
          }
        }

        groundTile.coords.push_back(Transform(points[idx],
                                              stateMap,
                                              cellMinLat,cellMinLon,
                                              true));

        groundTile.coords.push_back(Transform(pathEnd->point,
                                              stateMap,
                                              cellMinLat,cellMinLon,
                                              false));
      }
    }
    else {
      size_t targetIdx=pathEnd->prevWayPointIndex;

      for (size_t idx=pathStart->prevWayPointIndex+1;
          idx<=targetIdx;
          idx++) {
        groundTile.coords.push_back(Transform(points[idx],
                                              stateMap,
                                              cellMinLat,cellMinLon,
                                              true));
      }

      groundTile.coords.push_back(Transform(pathEnd->point,
                                            stateMap,
                                            cellMinLat,cellMinLon,
                                            false));
    }
  }

  /**
   * If intersection->direction==Direction::in, we are searching next OUT intersection,
   * previos IN intersection othervise.
   */
  WaterIndexProcessor::IntersectionRef WaterIndexProcessor::FindSiblingIntersection(const IntersectionRef &intersection,
                                                                                    const std::list<IntersectionRef> &intersectionsCW,
                                                                                    bool isArea)
  {
    Direction searchDirection=intersection->direction==Direction::in ? Direction::out : Direction::in;
    std::list<IntersectionRef> candidates;

    for (auto const &i:intersectionsCW) {
      if (intersection->coastline==i->coastline && i->direction==searchDirection) {
        candidates.push_back(i);
      }
    }

    IntersectionRef result;
    for (auto const &i:candidates) {
      if (intersection->direction==Direction::in) {
        if (i->prevWayPointIndex >= intersection->prevWayPointIndex) {
          if ((!result) || (result && i->prevWayPointIndex < result->prevWayPointIndex)) {
            result=i;
          }
        }
      }
      else {
        if (i->prevWayPointIndex <= intersection->prevWayPointIndex) {
          if ((!result) || (result && i->prevWayPointIndex > result->prevWayPointIndex)) {
            result=i;
          }
        }
      }
    }

    if (result || !isArea) {
      return result;
    }

    for (auto const &i:candidates) {
      if (intersection->direction==Direction::in) {
        if (i->prevWayPointIndex <= intersection->prevWayPointIndex) {
          if ((!result) || (result && i->prevWayPointIndex < result->prevWayPointIndex)) {
            result=i;
          }
        }
      }
      else {
        if (i->prevWayPointIndex >= intersection->prevWayPointIndex) {
          if ((!result) || (result && i->prevWayPointIndex > result->prevWayPointIndex)) {
            result=i;
          }
        }
      }
    }
    return result;
  }

  bool WaterIndexProcessor::WalkFromTripoint(GroundTile& groundTile,
                                             const StateMap& stateMap,
                                             const CellBoundaries& cellBoundaries,
                                             IntersectionRef& pathStart, // incoming path
                                             IntersectionRef& pathEnd,
                                             Data& data,
                                             const std::list<IntersectionRef>& intersectionsCW,
                                             const std::vector<size_t>& containingPaths)
  {
    CoastlineDataRef coastline=data.coastlines[pathStart->coastline];

    if (coastline->points.size()<2){
      return false;
    }

    GeoCoord   tripoint=(pathStart->direction==Direction::in) ? coastline->points.back() : coastline->points.front();
    GeoCoord   previousPoint=(pathStart->direction==Direction::in) ?
      coastline->points[coastline->points.size()-2] :
      coastline->points[1];
    CoastState walkType=(pathStart->direction==Direction::in) ? coastline->right : coastline->left;

    // try to find right outgoing path from the tripoint
    std::vector<size_t> candidates;

    for (const auto& intersection:intersectionsCW) {
      candidates.push_back(intersection->coastline);
    }

    // cell may fully contain path that is part of this tripoint
    for (const size_t path : containingPaths) {
      candidates.push_back(path);
    }

    IntersectionRef  outgoing;
    IntersectionRef  outgoingEnd;
    double           outgoingAngle=0;
    CoastlineDataRef outgoingCoastline;
    bool             intersectCell=false;

    for (const size_t pathIndex : candidates) {
      if (pathStart->coastline==pathIndex) {
        continue;
      }

      CoastlineDataRef path=data.coastlines[pathIndex];

      if (path->points.size()<2) {
        continue;
      }

      if (!tripoint.IsEqual(path->points.front()) && !tripoint.IsEqual(path->points.back())) {
        continue;
      }

      Direction direction=tripoint.IsEqual(path->points.front()) ? Direction::out : Direction::in;
      if ((direction==Direction::out && walkType!=path->right) ||
          (direction==Direction::in && walkType!=path->left)) {
        continue;
      }

      GeoCoord previousOutPoint= direction==Direction::out ? path->points[1] : path->points[path->points.size()-2];

      double angle=(tripoint.GetLon()-previousPoint.GetLon())*
                   (previousOutPoint.GetLat()-tripoint.GetLat())-
                   (tripoint.GetLat()-previousPoint.GetLat())*
                   (previousOutPoint.GetLon()-tripoint.GetLon());

      if (!outgoing || angle < outgoingAngle) {
        outgoingAngle=angle;
        outgoingCoastline=path;

        outgoing=std::make_shared<Intersection>();
        outgoing->coastline=pathIndex;
        outgoing->prevWayPointIndex=(direction==Direction::in) ? path->points.size()-1 : 0;
        outgoing->point=tripoint;
        outgoing->distanceSquare=0; // ?
        outgoing->direction=(direction==Direction::in) ? Direction::out : Direction::in; // in direction to tripoint == "outgoing" from cell
        outgoing->borderIndex=0; // ?

        IntersectionRef pathCellIntersection;

        for (const auto &cellIntersection:intersectionsCW) {
          if (cellIntersection->coastline==pathIndex) {
            if (!pathCellIntersection) {
              pathCellIntersection=cellIntersection;
            }
            else {
              if (direction==Direction::out &&
                  (pathCellIntersection->prevWayPointIndex > cellIntersection->prevWayPointIndex ||
                   (pathCellIntersection->prevWayPointIndex==cellIntersection->prevWayPointIndex &&
                    pathCellIntersection->distanceSquare > cellIntersection->distanceSquare))){
                pathCellIntersection=cellIntersection;
              }

              if (direction==Direction::in &&
                  (pathCellIntersection->prevWayPointIndex < cellIntersection->prevWayPointIndex ||
                   (pathCellIntersection->prevWayPointIndex==cellIntersection->prevWayPointIndex &&
                    pathCellIntersection->distanceSquare < cellIntersection->distanceSquare))){
                pathCellIntersection=cellIntersection;
              }
            }
          }
        }
        intersectCell=pathCellIntersection ? true : false;

        if (pathCellIntersection) {
          outgoingEnd=pathCellIntersection;
        }
        else {
          outgoingEnd=std::make_shared<Intersection>();
          outgoingEnd->coastline=pathIndex;
          outgoingEnd->prevWayPointIndex=(direction==Direction::in) ? 0 : path->points.size()-1;
          outgoingEnd->point=(direction==Direction::in) ? path->points.front() : path->points.back();
          outgoingEnd->distanceSquare=0; // ?
          outgoingEnd->direction=direction;
          outgoingEnd->borderIndex=0; // ?
        }
      }
    }

    if (!outgoing || outgoing->direction==outgoingEnd->direction) {
      return false;
    }

    // we are left this cell
    if (intersectCell) {
      pathEnd=outgoingEnd;
    }

    // finally, walk from the tripoint (outgoing) to (outgoingEnd)
    WalkPath(groundTile,
             stateMap,
             cellBoundaries,
             outgoing,
             outgoingEnd,
             outgoingCoastline);
    pathStart=outgoing;

    return true;
  }

  void WaterIndexProcessor::WalkPath(GroundTile &groundTile,
                                     const StateMap& stateMap,
                                     const CellBoundaries &cellBoundaries,
                                     const IntersectionRef pathStart,
                                     const IntersectionRef pathEnd,
                                     CoastlineDataRef coastline)
  {
#if defined(DEBUG_COASTLINE)
      std::cout << "     ... path from " << pathStart->point.GetDisplayText() <<
                                  " to " << pathEnd->point.GetDisplayText() << std::endl;
#endif
      if (pathStart->direction==Direction::out){
        WalkPathBack(groundTile,
                     stateMap,
                     cellBoundaries.latMin,
                     cellBoundaries.lonMin,
                     pathStart,
                     pathEnd,
                     coastline->points,
                     coastline->isArea);
      }else{
        WalkPathForward(groundTile,
                        stateMap,
                        cellBoundaries.latMin,
                        cellBoundaries.lonMin,
                        pathStart,
                        pathEnd,
                        coastline->points,
                        coastline->isArea);
      }
  }

  bool WaterIndexProcessor::WalkBoundaryCW(GroundTile &groundTile,
                                           const StateMap& stateMap,
                                           const IntersectionRef startIntersection,
                                           const std::list<IntersectionRef>& intersectionsCW,
                                           std::set<IntersectionRef>& visitedIntersections,
                                           const CellBoundaries& cellBoundaries,
                                           Data& data,
                                           const std::vector<size_t>& containingPaths)
  {
#if defined(DEBUG_COASTLINE)
      std::cout << "   walk around " << TypeToString(groundTile.type) <<
        " from " << startIntersection->point.GetDisplayText() << std::endl;
#endif

    groundTile.coords.push_back(Transform(startIntersection->point,
                                          stateMap,
                                          cellBoundaries.latMin,
                                          cellBoundaries.lonMin,
                                          false));

    IntersectionRef pathStart=startIntersection;
    bool            error=false;
    size_t          step=0;

    while ((step==0 || pathStart!=startIntersection) && !error) {
      visitedIntersections.insert(pathStart);
      CoastlineDataRef coastline=data.coastlines[pathStart->coastline]; // TODO: check that we have correct type
      IntersectionRef pathEnd=FindSiblingIntersection(pathStart,
                                                      intersectionsCW,
                                                      coastline->isArea);
      if (!pathEnd) {
#if defined(DEBUG_COASTLINE)
        std::cout << "     can't found sibling intersection for " << pathStart->point.GetDisplayText() << std::endl;
#endif
        GeoCoord tripoint=(pathStart->direction==Direction::in) ? coastline->points.back() : coastline->points.front();

        // create synthetic end
        IntersectionRef end=std::make_shared<Intersection>();
        end->coastline=pathStart->coastline;
        end->prevWayPointIndex=(pathStart->direction==Direction::in) ? coastline->points.size()-1 : 0;
        end->point=tripoint;
        end->distanceSquare=0; // ?
        end->direction=(pathStart->direction==Direction::in) ? Direction::out : Direction::in;
        end->borderIndex=0; // ?

        WalkPath(groundTile,
                 stateMap,
                 cellBoundaries,
                 pathStart,
                 end,
                 coastline);

        while (!pathEnd) {
          if (coastline->isArea) {
            return false; // area can't be part of tripoint, it should not happen
          }
#if defined(DEBUG_COASTLINE)
          GeoCoord tripoint=(pathStart->direction==Direction::in) ? coastline->points.back() : coastline->points.front();
          std::cout << "     found tripoint " << tripoint.GetDisplayText() << std::endl;
#endif

          // handle coastline Tripoint
          if (!WalkFromTripoint(groundTile,
                                stateMap,
                                cellBoundaries,
                                pathStart,
                                pathEnd,
                                data,
                                intersectionsCW,
                                containingPaths)) {
            return false;
          }

          step++;
          if (step>1000) { // last fuse
            // put breakpoint here if computation stucks in this loop :-/
            std::cout << "   too many steps, give up... " << step << std::endl;
            return false;
          }
        }
      }
      else {
        WalkPath(groundTile,
                 stateMap,
                 cellBoundaries,
                 pathStart,
                 pathEnd,
                 coastline);
      }

      step++;
      if (step>1000) { // last use
        // put breakpoint here if computation stucks in this loop :-/
        std::cout << "   too many steps, give up... " << step << std::endl;
        return false;
      }

      pathStart=GetNextCW(intersectionsCW,
                          pathEnd);

      WalkBorderCW(groundTile,
                   stateMap,
                   cellBoundaries.latMin,
                   cellBoundaries.lonMin,
                   pathEnd,
                   pathStart,
                   cellBoundaries.borderCoords);

    }


    return true;
  }

  void WaterIndexProcessor::HandleCoastlineCell(Progress& progress,
                                                const Pixel &cell,
                                                const std::list<size_t>& intersectCoastlines,
                                                const StateMap& stateMap,
                                                std::map<Pixel,std::list<GroundTile> >& cellGroundTileMap,
                                                Data& data)
  {
      std::list<IntersectionRef> intersectionsCW;        // Intersections in clock wise order over all coastlines
      std::set<IntersectionRef>  visitedIntersections;
      CellBoundaries             cellBoundaries(stateMap,cell);

      // For every coastline by index intersecting the current cell
      for (const auto& currentCoastline : intersectCoastlines) {
        CoastlineDataRef coastData=data.coastlines[currentCoastline];
        std::map<Pixel,std::list<IntersectionRef>>::iterator cellData=coastData->cellIntersections.find(cell);

        assert(cellData!=coastData->cellIntersections.end());

        intersectionsCW.insert(intersectionsCW.end(), cellData->second.begin(), cellData->second.end());
      }

      intersectionsCW.sort(IntersectionCWComparator());

      // collect fully contained coastline paths (may be part of tripoints)
      std::vector<size_t> containingPaths;

      const auto &cellCoastlineEntry=data.cellCoveredCoastlines.find(cell);

      if (cellCoastlineEntry!=data.cellCoveredCoastlines.end()) {
        for (size_t i : cellCoastlineEntry->second) {
          if (!data.coastlines[i]->isArea && data.coastlines[i]->isCompletelyInCell) {
            containingPaths.push_back(i);
          }
        }
      }

#if defined(DEBUG_COASTLINE)
      std::cout.precision(5);
      std::cout << "    cell boundaries" <<
        ": " << cellBoundaries.latMin << " " << cellBoundaries.lonMin <<
        "; " << cellBoundaries.latMin << " " << cellBoundaries.lonMax <<
        "; " << cellBoundaries.latMax << " " << cellBoundaries.lonMin <<
        "; " << cellBoundaries.latMax << " " << cellBoundaries.lonMax <<
        std::endl;
      std::cout << "    intersections:" << std::endl;
      for (const auto &intersection: intersectionsCW){
        std::cout << "      " << intersection->point.GetDisplayText() << " (" << intersection->coastline << ", ";
        std::cout << (intersection->direction==Direction::out ? "out" : "in");
        std::cout << ", " << intersection->prevWayPointIndex;
        std::cout << ")" << std::endl;
      }
#endif

      for (const auto& intersection : intersectionsCW) {
        if (intersection->direction==Direction::touch) {
          continue; // TODO: what to do?
        }

        if (visitedIntersections.find(intersection)!=visitedIntersections.end()) {
          continue;
        }

        CoastlineDataRef coastline=data.coastlines[intersection->coastline];

        CoastState coastState=intersection->direction==Direction::in?coastline->right:coastline->left;
        assert(coastState!=CoastState::undefined);

        GroundTile groundTile(GroundTile::Type::unknown);

        if (coastState==CoastState::land) {
          groundTile.type=GroundTile::Type::land;
        }
        else if (coastState==CoastState::water) {
          groundTile.type=GroundTile::Type::water;
        }

        if (!WalkBoundaryCW(groundTile,
                            stateMap,
                            intersection,
                            intersectionsCW,
                            visitedIntersections,
                            cellBoundaries,
                            data,
                            containingPaths)) {
            progress.Warning("Can't walk around cell boundary!");
            continue;
        }

        cellGroundTileMap[cell].push_back(groundTile);
      }
  }

  /**
   * Fills coords information for cells that intersect a coastline
   */
  void WaterIndexProcessor::HandleCoastlinesPartiallyInACell(Progress& progress,
                                                             const StateMap& stateMap,
                                                             std::map<Pixel,std::list<GroundTile> >& cellGroundTileMap,
                                                             Data& data)
  {
    progress.Info("Handle coastlines partially in a cell");

    // For every cell with intersections
    size_t currentCell=0;
    for (const auto& cellEntry : data.cellCoastlines) {
      progress.SetProgress(currentCell,data.cellCoastlines.size());
      currentCell++;

#if defined(DEBUG_COASTLINE)
      std::cout << " - cell " << cellEntry.first.x << " " << cellEntry.first.y <<
        " (level offset " << level.indexEntryOffset << "): " << std::endl;
#endif

      HandleCoastlineCell(progress,
                          cellEntry.first,
                          cellEntry.second,
                          stateMap,
                          cellGroundTileMap,
                          data);
    }
  }

  void WaterIndexProcessor::CalculateHasCellData(Level& level,
                                                 const std::map<Pixel,std::list<GroundTile>>& cellGroundTileMap) const
  {
    level.hasCellData=false;
    level.defaultCellData=unknown;

    if (level.stateMap.GetXCount()>0 && level.stateMap.GetYCount()>0) {
      level.defaultCellData=level.stateMap.GetState(0,0);

      if (cellGroundTileMap.size()>0) {
        level.hasCellData=true;
      }
      else {
        for (uint32_t y=0; y<level.stateMap.GetYCount(); y++) {
          for (uint32_t x=0; x<level.stateMap.GetXCount(); x++) {
            level.hasCellData=level.stateMap.GetState(x,y)!=level.defaultCellData;

            if (level.hasCellData) {
              break;
            }
          }

          if (level.hasCellData) {
            break;
          }
        }
      }
    }
  }

  /**
   * try to synthetize coastline segments from all way coastlines
   * that intersect with the bounding polygon
   *
   * @param progress
   * @param coastlines
   * @param boundingPolygons
   */
  void WaterIndexProcessor::SynthesizeCoastlines(Progress& progress,
                                                 std::list<CoastRef>& coastlines,
                                                 std::list<CoastRef>& boundingPolygons)
  {
    progress.SetAction("Synthetize coastlines");

    std::list<CoastRef> allCoastlines(coastlines);
    std::list<CoastRef> synthesized;

    osmscout::StopClock clock;
    SynthesizeCoastlines2(progress,
                          boundingPolygons,
                          allCoastlines,
                          synthesized);

    // define coastline states if there are still some undefined
    for (const auto& coastline : synthesized) {
      if (coastline->right==CoastState::undefined) {
        coastline->right=CoastState::unknown;
      }

      if (coastline->left==CoastState::undefined && coastline->isArea) {
        for (const auto& testCoast : synthesized) {
          if (testCoast->right==CoastState::water &&
              IsAreaAtLeastPartlyInArea(testCoast->coast,coastline->coast)) {
            coastline->left=CoastState::water;
          }
        }
      }

      if (coastline->left==CoastState::undefined) {
        // still undefined, it is land probably
        coastline->left=CoastState::land;
      }
    }

    clock.Stop();
    progress.Info(NumberToString(boundingPolygons.size())+" bouding polygon(s), and "+
                  NumberToString(allCoastlines.size())+" coastline(s) synthesized into "+
                  NumberToString(synthesized.size())+" coastlines(s), took "+
                  clock.ResultString() +" s"
                 );

    coastlines=synthesized;
  }

  /**
   * Take the given coastlines and bounding polygons and create a list of synthesized
   * coastlines that fuly encircle the imported region. Coastlines are either
   * real-life coastlines or emulated coastlines based on the bounding
   * polygons.
   *
   * @param progress
   * @param boundingPolygons
   * @param coastlines
   * @param synthesized
   */
  void WaterIndexProcessor::SynthesizeCoastlines2(Progress& progress,
                                                  const std::list<CoastRef>& boundingPolygons,
                                                  const std::list<CoastRef>& coastlines,
                                                  std::list<CoastRef>& synthesized)
  {
    std::vector<CoastRef> candidates;

    for (const auto& polygon : boundingPolygons) {
      CoastRef candidate=std::make_shared<Coast>();

      candidate->isArea=true;
      candidate->coast=polygon->coast;
      candidate->left=polygon->left;
      candidate->right=polygon->right;
      candidates.push_back(candidate);
    }

    std::vector<std::vector<PathIntersection>> wayIntersections(coastlines.size()); // List of intersections for each coastline

    /**
     * create matrix of intersections between bounding polygons and coastlines
     * split candidate and ways separately
     */
    for (const auto& c : candidates) {
      //WriteGpx(c->coast,"candidate"+NumberToString(ci)+".gpx");
      std::vector<PathIntersection> candidateIntersections;

      size_t wi=0;
      for (const auto& coastline : coastlines) {

        //WriteGpx(coastline->coast,"coastline-"+NumberToString(coastline->id)+".gpx");
        // try to find intersections between this candidate and way
        std::vector<PathIntersection> intersections;

        FindPathIntersections(c->coast,
                              coastline->coast,
                              c->isArea,
                              coastline->isArea,
                              intersections);

        // filter out intersections when part of coastline and bounding polygon area is same
        // intersection.orientation==0.0 (paths only touch but do not cross each other)
        size_t valid=0;

        for (auto &intersection:intersections) {
          if (intersection.orientation!=0.0) {
            candidateIntersections.push_back(intersection);
            wayIntersections[wi].push_back(intersection);
            valid++;
          }
        }

        if (valid%2!=0) {
          progress.Warning("Odd count ("+NumberToString(valid)+") of valid intersections. "+
                           "Coastline "+NumberToString(coastline->id));
        }

        wi++;
      }

      // cut candidate
      if (candidateIntersections.empty()) {
        synthesized.push_back(c);
      }
      else {
        if (candidateIntersections.size()%2!=0) {
          progress.Warning("Odd count of intersections: "+candidateIntersections.size());
          continue;
        }

        std::sort(candidateIntersections.begin(),
                  candidateIntersections.end(),
                  PathIntersectionSortA);

        for (size_t ii=0; ii<candidateIntersections.size(); ii++) {
          PathIntersection int1=candidateIntersections[ii];
          PathIntersection int2=candidateIntersections[(ii+1)%candidateIntersections.size()];

#if defined(DEBUG_COASTLINE)
          std::cout.precision(5);
          std::cout << "    Cut data polygon from " <<
            int1.point.GetLat() << " " << int1.point.GetLon() << " to " <<
            int2.point.GetLat() << " " << int2.point.GetLon() << " left state: " <<
            (int1.orientation>0 ?"water":"land") <<
            std::endl;
#endif

          CoastRef part=std::make_shared<Coast>();

          part->coast.push_back(Point(0,int1.point));

          CutPath(part->coast,
                  c->coast,
                  int1.aIndex+1,
                  int2.aIndex+1,
                  int1.aDistanceSquare,
                  int2.aDistanceSquare);

          part->coast.push_back(Point(0,int2.point));

          part->left=int1.orientation>0 ? CoastState::water : CoastState::land;

          assert(int1.orientation>0 ? int2.orientation<0 : int2.orientation>0);

          part->right=c->right;
          part->id=c->id;
          part->sortCriteria=c->sortCriteria;
          part->isArea=false;

          synthesized.push_back(part);

          //WriteGpx(part->coast,"data.gpx");
        }
      }
    }

    // cut ways
    size_t wi=0;
    for (const auto& coastline : coastlines) {
      std::vector<PathIntersection> &intersections=wayIntersections[wi];

      wi++;
      if (intersections.empty()) {
        if (coastline->isArea) {
          // test island without intersections if it is inside data polygon...
          for (const auto& poly : boundingPolygons) {
            if (IsAreaAtLeastPartlyInArea(coastline->coast,
                                          poly->coast)) {
              synthesized.push_back(coastline);
              break;
            }
          }
        }

        continue;
      }

      if (intersections.size()%2!=0) {
        progress.Warning("Odd count of intersections: "+intersections.size());
        continue;
      }

      std::sort(intersections.begin(),
                intersections.end(),
                PathIntersectionSortB);

      size_t limit=coastline->isArea?intersections.size():intersections.size()-1;

      for (size_t ii=0; ii<limit; ii++) {
        PathIntersection int1=intersections[ii];
        PathIntersection int2=intersections[(ii+1)%intersections.size()];

        assert(int1.orientation>0 ? int2.orientation<0 : int2.orientation>0);

        if (int1.orientation<0){
          continue;
        }

#if defined(DEBUG_COASTLINE)
        std::cout.precision(5);
        std::cout << "    Cut coastline from " <<
          int1.point.GetLat() << " " << int1.point.GetLon() << " to " <<
          int2.point.GetLat() << " " << int2.point.GetLon() <<
          std::endl;
#endif

        CoastRef part=std::make_shared<Coast>();

        part->coast.push_back(Point(0,int1.point));

        CutPath(part->coast,coastline->coast,
                int1.bIndex+1,int2.bIndex+1,
                int1.bDistanceSquare,int2.bDistanceSquare);

        part->coast.push_back(Point(0,int2.point));
        part->left=coastline->left;
        part->right=coastline->right;
        part->id=coastline->id;
        part->sortCriteria=coastline->sortCriteria;
        part->isArea=false;

        synthesized.push_back(part);

        //WriteGpx(part->coast,"cut.gpx");
      }
    }
  }

  void WaterIndexProcessor::WriteTiles(Progress& progress,
                                       const std::map<Pixel,std::list<GroundTile>>& cellGroundTileMap,
                                       Level& level,
                                       FileWriter& writer)
  {
    if (level.hasCellData) {

      //
      // Calculate size of data
      //

      size_t dataSize=4;
      char   buffer[10];

      for (const auto& coord : cellGroundTileMap) {
        // Number of ground tiles
        dataSize+=EncodeNumber(coord.second.size(),buffer);

        for (const auto& tile : coord.second) {
          // Type
          dataSize++;

          // Number of coordinates
          dataSize+=EncodeNumber(tile.coords.size(),buffer);

          // Data for coordinate pairs
          dataSize+=tile.coords.size()*2*sizeof(uint16_t);
        }
      }

      level.dataOffsetBytes=BytesNeededToEncodeNumber(dataSize);

      progress.Info("Writing index for level "+
                    NumberToString(level.level)+", "+
                    NumberToString(level.stateMap.GetXCount()*level.stateMap.GetYCount())+" cells, "+
                    NumberToString(cellGroundTileMap.size())+" entries, "+
                    NumberToString(level.dataOffsetBytes)+" bytes/entry, "+
                    ByteSizeToString(1.0*level.stateMap.GetXCount()*level.stateMap.GetYCount()*level.dataOffsetBytes+dataSize));

      //
      // Write bitmap
      //

      level.indexDataOffset=writer.GetPos();

      for (uint32_t y=0; y<level.stateMap.GetYCount(); y++) {
        for (uint32_t x=0; x<level.stateMap.GetXCount(); x++) {
          State state=level.stateMap.GetState(x,y);

          writer.WriteFileOffset((FileOffset) state,
                                 level.dataOffsetBytes);
        }
      }

      //
      // Write data
      //

      FileOffset dataOffset=writer.GetPos();

      // TODO: when data format will be changing, consider usage ones (0xFF..FF) as empty placeholder
      writer.WriteFileOffset((FileOffset)0,4);

      for (const auto& coord : cellGroundTileMap) {
        FileOffset startPos=writer.GetPos();

        writer.WriteNumber((uint32_t) coord.second.size());

        for (const auto& tile : coord.second) {
          writer.Write((uint8_t) tile.type);

          writer.WriteNumber((uint32_t) tile.coords.size());

          for (size_t c=0; c<tile.coords.size(); c++) {
            if (tile.coords[c].coast) {
              uint16_t x=tile.coords[c].x | uint16_t(1 << 15);

              writer.Write(x);
            }
            else {
              writer.Write(tile.coords[c].x);
            }
            writer.Write(tile.coords[c].y);
          }
        }

        FileOffset endPos;
        uint32_t   cellId=coord.first.y*level.stateMap.GetXCount()+coord.first.x;
        size_t     index =cellId*level.dataOffsetBytes;

        endPos=writer.GetPos();

        writer.SetPos(level.indexDataOffset+index);
        writer.WriteFileOffset(startPos-dataOffset,
                               level.dataOffsetBytes);
        writer.SetPos(endPos);
      }
    }
    else {
      progress.Info("All cells have state '"+StateToString(level.defaultCellData)+"' and no coastlines, no cell index needed");
    }

    FileOffset currentPos=writer.GetPos();

    writer.SetPos(level.indexEntryOffset);
    writer.Write(level.hasCellData);
    writer.Write(level.dataOffsetBytes);
    writer.Write((uint8_t) level.defaultCellData);
    writer.WriteFileOffset(level.indexDataOffset);
    writer.SetPos(currentPos);
  }
}