#ifndef INCLUDE_MAVLINK_ROS_OPERATOR_CONTROLLER_H_
#define INCLUDE_MAVLINK_ROS_OPERATOR_CONTROLLER_H_

#include <control/pid.h>
#include <cpputils/Timer.h>
#include <mavlink_cpp/GCS.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <mavlink_ros_msgs/pid_debug.h>

namespace mavlink_ros {

using namespace cpputils;
using namespace mavlink_cpp;
using namespace control;

class OperatorController : public Logger {
public:
  struct Params {
    bool sitl, use_tf;
    std::string desired_robot_tf;
    std::string robot_tf;
    std::string ref_tf;
  };

  struct ControlState {
    double x, y, z, r;
    FLY_MODE_R mode;
    bool arm;
  };

  OperatorController(const Params &params);
  void Start();
  ControlState ControlState;
  std::shared_ptr<GCS> Control;

  void ResetPID();
  void SetTfMode(const bool &tfmode);
  void SetReferenceTfName(const std::string &ref);
  void SetRobotTfName(const std::string &ref);
  void SetDesiredPosTfName(const std::string &ref);

  void SetnedMerov(const tf::Transform & transform);
  void SetnedMtarget(const tf::Transform & transform);

private:
  tf::TransformListener listener;
  geometry_msgs::TransformStamped static_transformStamped;
  std::vector<geometry_msgs::TransformStamped> static_transforms;

  tf::StampedTransform cameraMrov;
  tf::Transform rovMtarget, _nedMtarget, _nedMerov;
  tf::Transform rovMned;
  ros::Publisher debugPublisher0;

  cpputils::Timer timer;

  // PID
  double vmax = 1000, vmin = -1000;
  PID yawPID, xPID, yPID, zPID;
  double yoffset, xoffset, roffset, zoffset, deadband, baseZ, zoffsetPos;
  // End PID

  Params _params;

  // FUNCTIONS
  double keepHeadingIteration(const double &dt, double diff);
  double ArduSubXYR(double per);
  double ArduSubZ(double per);
  void Saturate(const double &max, const double &x, const double &y,
                const double &z, double &vx, double &vy, double &vz);
  void GetLinearXVel(const double &dt, const double &diff, double &v);
  void GetLinearYVel(const double &dt, const double &diff, double &v);
  void GetLinearZVel(const double &dt, const double &diff, double &v);
  void Loop();

  bool GetnedMerov(tf::StampedTransform &);
  bool GetnedMtarget(tf::StampedTransform &);

  std::thread _mainLoop;
  bool nedMerovUpdated = false, nedMtargetUpdated = false;

  std::mutex nedMerov_mutex, nedMtarget_mutex;
  std::condition_variable nedMerov_cond, nedMtarget_cond;

  mavlink_ros_msgs::pid_debug pid_debug_msg;
  ros::Publisher pid_debug_publisher;
};

} // namespace mavlink_ros

#endif /* INCLUDE_MAVLINK_ROS_OPERATOR_CONTROLLER_H_ */
