// A stock car's physics rows from the merged game database, as raw columns,
// and the game-wide PhysicsSettings.ini tunables. The drive model turns
// them into its own parameters (libs/drive, from_physics.h); this side only
// finds and carries the rows.
// Spec: SPEC-CAR-PHYSICS (Car parameters: how a stock car's rows are found;
// Global settings); FORMAT-CARS (DLC packages: database rows).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cars/sources.h"

namespace cars {

// Values are keyed "<group>.<column>", the group naming the row:
//   Data_Car, Data_CarBody, Data_Engine, Data_Drivetrain, and each stock
//   upgrade row by its table (List_UpgradeEngineCamshaft, ...);
//   List_TorqueCurve (the camshaft's full-throttle curve, scalars);
//   FrontSpringDamper, RearSpringDamper, FrontAntiSway, RearAntiSway;
//   List_TireCompound, LatMultiCurve, LongMultiCurve.
// Curves: "torque" (List_TorqueCurve v0..), "lat0", "lat1", "long0",
// "long1" (List_TireFrictionCurve v0.. of the multi curves' ID0 and ID1).
// Only numeric columns are kept. A stock row that does not exist (a car
// without a turbo) leaves its group out.
struct CarPhysics {
  int64_t car_id = 0;
  std::string media_name;
  std::map<std::string, double> values;
  std::map<std::string, std::vector<double>> curves;

  bool has(const std::string& key) const { return values.count(key) != 0; }
  double get(const std::string& key, double fallback = 0) const {
    auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
  }
  const std::vector<double>* curve(const std::string& name) const {
    auto it = curves.find(name);
    return it == curves.end() ? nullptr : &it->second;
  }
};

// PhysicsSettings.ini: "Section\Key value" lines; numeric values only,
// keyed as written ("SidewallScalePeakSA\TireSidewall0").
struct PhysicsSettings {
  std::map<std::string, double> values;
  double get(const std::string& key, double fallback) const {
    auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
  }
};

bool parse_physics_settings(std::string_view text, PhysicsSettings& out, std::string* error);
// media/physics.zip member PhysicsSettings.ini.
bool load_physics_settings(const std::filesystem::path& physics_zip, PhysicsSettings& out, std::string* error);

// The stock rows of each car in `ids` (Data_Car.Id) over the merged
// database (load_database's merge). A car whose links do not resolve is an
// error naming the link.
bool load_car_physics(const Install& install, const std::vector<int64_t>& ids, std::vector<CarPhysics>& out,
                      std::string* error);

// SPEC-CAMERAS' per-car inputs: the car's CameraOverrides row (by CarId;
// every column, zero when the car has no row) and its collision box,
// MAXData.xml Collision/BoundingBox from the car archive (MinX MinY MinZ
// MaxX MaxY MaxZ as written: x and y in the car frame, z before the
// BottomCenterWheelbasePosZ shift).
struct CarCameraRow {
  std::map<std::string, double> overrides;  // column name -> value
  bool has_overrides = false;
  std::array<double, 6> box{};
  bool has_box = false;
};
bool load_car_camera(const Install& install, int64_t car_id, const std::string& media, CarCameraRow& out,
                     std::string* error);
// media/camera.zip's CameraSettings.ini and CameraPhysics.xml; v4's own
// camera/CameraPhysics.xml from its media.zip when there is one (SPEC-
// CAMERAS, Inputs).
bool load_camera_files(const Install& install, std::string& settings_ini, std::string& physics_xml,
                       std::string* error);

// A text form for the bake: "value <key> <number>", "curve <name> <n>
// <numbers...>", "setting <key> <number>" and "car <id> <media name>".
std::string physics_to_text(const CarPhysics& car, const PhysicsSettings& settings);
bool physics_from_text(std::string_view text, CarPhysics& car, PhysicsSettings& settings, std::string* error);

}  // namespace cars
