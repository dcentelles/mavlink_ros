#include <chrono>
#include <control/pid.h>
#include <cpputils/Timer.h>
#include <mavlink_cpp/GCS.h>
#include <mavlink_ros/OperatorController.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>

namespace mavlink_ros {

OperatorController::OperatorController(
    const OperatorController::Params &params) {
  SetLogName("GCS");
  SetLogLevel(info);
  SetAsyncMode();
  FlushLogOn(debug);
  SetLogFormatter(std::make_shared<spdlog::pattern_formatter>("[%T.%F] %v"));
  SetAsyncMode(true);
  uint16_t localPort = 14550;
  Control = std::shared_ptr<GCS>(new GCS(localPort));
  Control->SetLogName("GCS");
  Control->SetLogLevel(info);

  Control->EnableGPSMock(false);
  Control->SetManualControl(0, 0, 0, 0);
  Control->EnableManualControl(true);
  Control->Start();

  ControlState.arm = false;
  ControlState.mode = FLY_MODE_R::MANUAL;

  _params.sitl = params.sitl;
  _params.use_tf = params.use_tf;
  _params.desired_robot_tf = params.desired_robot_tf != ""
                                 ? params.desired_robot_tf
                                 : "bluerov2_ghost";
  _params.robot_tf = params.robot_tf != "" ? params.robot_tf : "erov";
  _params.ref_tf = params.ref_tf != "" ? params.ref_tf : "local_origin_ned";

  ros::NodeHandle nh("operator_controller");
  pid_debug_publisher =
      nh.advertise<mavlink_ros_msgs::pid_debug>("pid_debug", 1);
}

void OperatorController::SetReferenceTfName(const std::string &ref) {
  _params.ref_tf = ref;
}
void OperatorController::SetRobotTfName(const std::string &ref) {
  _params.robot_tf = ref;
}
void OperatorController::SetDesiredPosTfName(const std::string &ref) {
  _params.desired_robot_tf = ref;
}

void OperatorController::Start() {
  if (_params.sitl) {
    yawPID.SetConstants(vmax, vmin, 10, 20, 0.05);
    xPID.SetConstants(vmax, vmin, 10, 60, 0.05);
    yPID.SetConstants(vmax, vmin, 10, 60, 0.05);
    zPID.SetConstants(vmax, vmin, 20, 10, 0.1);
    baseZ = -77;
    yoffset = 60;
    xoffset = 60;
    roffset = 400;
    zoffset = 10;
    deadband = 0;
    zoffsetPos = 0;
  } else {
    yawPID.SetConstants(vmax, vmin, 10, 20, 0.05);
    xPID.SetConstants(vmax, vmin, 20, 60, 0.05);
    yPID.SetConstants(vmax, vmin, 20, 60, 0.05);
    zPID.SetConstants(vmax, vmin, 20, 10, 0.05);
    baseZ = -20;
    yoffset = 45;  // 90;
    xoffset = 45;  // 90;
    roffset = 440; // 460;
    zoffset = 10;
    deadband = 0;
    zoffsetPos = 100;
  }

  _mainLoop = std::thread([this]() { Loop(); });
}

void OperatorController::ResetPID() {
  yawPID.Reset();
  xPID.Reset();
  yPID.Reset();
  zPID.Reset();
  timer.Reset();
}

void OperatorController::SetTfMode(const bool &tfmode) {
  _params.use_tf = tfmode;
}

void OperatorController::SetnedMerov(const tf::Transform &transform) {
  std::unique_lock<std::mutex> lock(nedMerov_mutex);
  _nedMerov = transform;
  nedMerovUpdated = true;
  nedMerov_cond.notify_all();
}

void OperatorController::SetnedMtarget(const tf::Transform &transform) {
  std::unique_lock<std::mutex> lock(nedMtarget_mutex);
  _nedMtarget = transform;
  nedMtargetUpdated = true;
  nedMtarget_cond.notify_all();
}

bool OperatorController::GetnedMerov(tf::StampedTransform &transform) {
  std::unique_lock<std::mutex> lock(nedMerov_mutex);
  if (!nedMerovUpdated) {
    nedMerov_cond.wait_for(lock, std::chrono::milliseconds(200));
  }
  if (nedMerovUpdated) {
    transform = tf::StampedTransform(_nedMerov, ros::Time::now(),
                                     _params.ref_tf, _params.robot_tf);
    nedMerovUpdated = false;
    return true;
  }
  return false;
}

bool OperatorController::GetnedMtarget(tf::StampedTransform &transform) {
  std::unique_lock<std::mutex> lock(nedMtarget_mutex);
  if (!nedMtargetUpdated) {
    nedMtarget_cond.wait_for(lock, std::chrono::milliseconds(200));
  }
  if (nedMtargetUpdated) {
    transform = tf::StampedTransform(_nedMtarget, ros::Time::now(),
                                     _params.ref_tf, _params.desired_robot_tf);
    nedMtargetUpdated = false;
    return true;
  }
  return false;
}

void OperatorController::Loop() {
  bool manual = true;
  tf::Transform rovMtarget;
  tf::Transform rovMned;
  tf::StampedTransform nedMerov, nedMtarget;

  double tyaw, cyaw;
  while (ros::ok()) {
    if (ControlState.mode == GUIDED && ControlState.arm) {
      if (manual) {
        Info("GUIDED ON");
        ResetPID();
        manual = false;
      }
      if (_params.use_tf) {
        try {
          listener.lookupTransform(_params.ref_tf, _params.robot_tf,
                                   ros::Time(0), nedMerov);
          listener.lookupTransform(_params.ref_tf, _params.desired_robot_tf,
                                   ros::Time(0), nedMtarget);
        } catch (tf::TransformException &ex) {
          Warn("Unable to get position info: {}", ex.what());
          Control->Arm(false);
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }
      } else {
        if (!GetnedMerov(nedMerov) || !GetnedMtarget(nedMtarget)) {
          Warn("Unable to get position info: rov position or target position "
               "unavailable");
          Control->Arm(false);
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }
      }
      Control->SetFlyMode(FLY_MODE_R::STABILIZE);
      Control->Arm(true);

      tf::Vector3 nedTerov = nedMerov.getOrigin();
      cyaw = tf::getYaw(nedMerov.getRotation());

      tf::Vector3 nedTtarget = nedMtarget.getOrigin();
      tyaw = tf::getYaw(nedMtarget.getRotation());

      rovMtarget = nedMerov.inverse() * nedMtarget;

      tf::Vector3 erovTtarget = rovMtarget.getOrigin();
      tf::Quaternion erovRtarget = rovMtarget.getRotation();

      double vx, vy, vz;
      double vTlpX = erovTtarget.getX(), vTlpY = erovTtarget.getY(),
             vTlpZ = erovTtarget.getZ();

      double elapsedSecs = timer.Elapsed() / 1000.;

      GetLinearXVel(elapsedSecs, vTlpX, vx);
      GetLinearYVel(elapsedSecs, vTlpY, vy);
      GetLinearZVel(elapsedSecs, -vTlpZ, vz);

      if (vx > 100)
        vx = 100;
      if (vx < -100)
        vx = -100;
      if (vy > 100)
        vy = 100;
      if (vy < -100)
        vy = -100;
      if (vz > 100)
        vz = 100;
      if (vz < -100)
        vz = -100;

      double rdiff = tf::getYaw(erovRtarget);
      double rv0 = keepHeadingIteration(elapsedSecs, rdiff);

      double newZ = vz + baseZ;
      auto x = ceil(ArduSubXYR(vx));
      auto y = ceil(ArduSubXYR(vy));
      auto z = ceil(ArduSubZ(newZ));
      auto r = ceil(ArduSubXYR(rv0));

      if (y > deadband)
        y += yoffset;
      else if (y < -deadband)
        y -= yoffset;

      if (x > deadband)
        x += xoffset;
      else if (x < -deadband)
        x -= xoffset;

      if (z > 500)
        z += zoffsetPos;
      else if (z < 500)
        z -= zoffset;

      if (r > deadband)
        r += roffset + 5;
      else if (r < -deadband)
        r -= roffset;

      Info("T.DIST: {}", nedTerov.distance(nedTtarget));
      Info("Send order: X: {} ({}) ; Y: {} ({}) ; Z: {} ({}) ; R: {} "
           ";  rdiff: "
           "{} ; rout: {} "
           "; rinput: {}",
           x, vx, y, vy, z, vz, r, rdiff, rv0, rv0);

      Control->SetManualControl(x, y, z, r);

      pid_debug_msg.pout_yaw = r;
      pid_debug_msg.pout_x = x;
      pid_debug_msg.pout_y = y;
      pid_debug_msg.pout_z = z;
      pid_debug_msg.error_yaw = rdiff;
      pid_debug_msg.error_x = vTlpX;
      pid_debug_msg.error_y = vTlpY;
      pid_debug_msg.error_z = -vTlpZ;
      pid_debug_msg.target_yaw = tyaw;
      pid_debug_msg.target_x = nedTtarget.getX();
      pid_debug_msg.target_y = nedTtarget.getY();
      pid_debug_msg.target_z = nedTtarget.getZ();
      pid_debug_msg.current_yaw = cyaw;
      pid_debug_msg.current_x = nedTerov.getX();
      pid_debug_msg.current_y = nedTerov.getY();
      pid_debug_msg.current_z = nedTerov.getZ();

      pid_debug_publisher.publish(pid_debug_msg);

      timer.Reset();
    } else {
      manual = true;
      FLY_MODE_R mode;
      switch (ControlState.mode) {
      case FLY_MODE_R::DEPTH_HOLD:
        mode = FLY_MODE_R::DEPTH_HOLD;
        break;
      case FLY_MODE_R::STABILIZE:
        mode = FLY_MODE_R::STABILIZE;
        break;
      case FLY_MODE_R::MANUAL:
        mode = FLY_MODE_R::MANUAL;
        break;
      default:
        mode = FLY_MODE_R::STABILIZE;
        break;
      }
      Control->SetFlyMode(mode);
      Control->Arm(ControlState.arm);
      auto x = ceil(ArduSubXYR(ControlState.x));
      auto y = ceil(ArduSubXYR(ControlState.y));
      auto z = ceil(ArduSubZ(ControlState.z));
      auto r = ceil(ArduSubXYR(ControlState.r));
      Control->SetManualControl(x, y, z, r);
      Info("Send order: X: {} ; Y: {} ; Z: {} ; R: {} ; Arm: {}", x, y, z, r,
           ControlState.arm ? "true" : "false");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

// from -100,100 to -1000,1000
double OperatorController::ArduSubXYR(double per) { return per * 10; }
double OperatorController::ArduSubZ(double per) { return (per + 100) / 0.2; }

double OperatorController::keepHeadingIteration(const double &dt, double diff) {
  double vel = yawPID.calculate(dt, 0, -1 * diff);

  return vel;
}

void OperatorController::Saturate(const double &max, const double &x,
                                  const double &y, const double &z, double &vx,
                                  double &vy, double &vz) {
  double rx, ry, rz;
  rx = x / max;
  ry = y / max;
  rz = y / max;
  if (rx > 1 || ry > 1 || rz > 1) {
    double alpha = rx;
    if (alpha < ry)
      alpha = ry;
    if (alpha < rz)
      alpha = rz;
    vx = x / alpha;
    vy = y / alpha;
    vz = z / alpha;
  } else {
    vx = x;
    vy = y;
    vz = z;
  }
}

void OperatorController::GetLinearXVel(const double &dt, const double &diffx,
                                       double &vx) {
  vx = xPID.calculate(dt, 0, -diffx);
}
void OperatorController::GetLinearYVel(const double &dt, const double &diffy,
                                       double &vy) {
  vy = yPID.calculate(dt, 0, -diffy);
}
void OperatorController::GetLinearZVel(const double &dt, const double &diffz,
                                       double &vz) {
  vz = zPID.calculate(dt, 0, -diffz);
}

} // namespace mavlink_ros
