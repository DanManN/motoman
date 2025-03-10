/*
 * Software License Agreement (BSD License)
 *
 * Copyright (c) 2013, Southwest Research Institute
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *  notice, this list of conditions and the following disclaimer in the
 *  documentation and/or other materials provided with the distribution.
 *  * Neither the name of the Southwest Research Institute, nor the names
 *  of its contributors may be used to endorse or promote products derived
 *  from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "motoman_driver/joint_trajectory_streamer.h"
#include "motoman_driver/simple_message/messages/motoman_motion_reply_message.h"
#include "simple_message/messages/joint_traj_pt_full_message.h"
#include "motoman_driver/simple_message/messages/joint_traj_pt_full_ex_message.h"
#include "industrial_robot_client/utils.h"
#include "industrial_utils/param_utils.h"
#include <map>
#include <vector>
#include <string>

#include "std_msgs/UInt32.h"

namespace CommTypes = industrial::simple_message::CommTypes;
namespace ReplyTypes = industrial::simple_message::ReplyTypes;
using industrial::joint_data::JointData;
using industrial::joint_traj_pt_full::JointTrajPtFull;
using industrial::joint_traj_pt_full_message::JointTrajPtFullMessage;
using industrial::joint_traj_pt_full_ex::JointTrajPtFullEx;
using industrial::joint_traj_pt_full_ex_message::JointTrajPtFullExMessage;
using industrial::shared_types::shared_int;

using motoman::simple_message::motion_reply_message::MotionReplyMessage;
namespace TransferStates = industrial_robot_client::joint_trajectory_streamer::TransferStates;
namespace MotionReplyResults = motoman::simple_message::motion_reply::MotionReplyResults;

namespace motoman
{
namespace joint_trajectory_streamer
{

namespace
{
  const double pos_stale_time_ = 1.0;  // max time since last "current position" update, for validation (sec)
  const double start_pos_tol_  = 1e-4;  // max difference btwn start & current position, for validation (rad)
}

#define ROS_ERROR_RETURN(rtn, ...) do {ROS_ERROR(__VA_ARGS__); return(rtn);} while (0)  // NOLINT(whitespace/braces)

void MotomanJointTrajectoryStreamer::run()
{
  /* ros::Publisher queue_size_pub = node_.advertise<std_msgs::UInt32>("robot_streaming_queue_size", 10); */
  ros::Publisher queue_size_pub = node_.advertise<std_msgs::UInt32>("robot_transfer_state", 10);
  ros::Rate loop_rate(100);
  unsigned int val;
  while (ros::ok()) {
    /* val = this->ptstreaming_queue_.size(); */
    val = this->state_;
    std_msgs::UInt32 msg;
    msg.data = val;
    queue_size_pub.publish(msg);
    ros::spinOnce();
    loop_rate.sleep();
  }
}

// override init() to read "robot_id" parameter and subscribe to joint_states
bool MotomanJointTrajectoryStreamer::init(SmplMsgConnection* connection, const std::map<int, RobotGroup> &robot_groups,
    const std::map<std::string, double> &velocity_limits)
{
  bool rtn = true;

  ROS_INFO("MotomanJointTrajectoryStreamer: init");

  this->robot_groups_ = robot_groups;
  rtn &= JointTrajectoryStreamer::init(connection, robot_groups, velocity_limits);

  motion_ctrl_.init(connection, 0);
  for (size_t i = 0; i < robot_groups_.size(); i++)
  {
    MotomanMotionCtrl motion_ctrl;

    int robot_id = robot_groups_[i].get_group_id();
    rtn &= motion_ctrl.init(connection, robot_id);

    motion_ctrl_map_[robot_id] = motion_ctrl;
  }

  disabler_ = node_.advertiseService("robot_disable", &MotomanJointTrajectoryStreamer::disableRobotCB, this);

  enabler_ = node_.advertiseService("robot_enable", &MotomanJointTrajectoryStreamer::enableRobotCB, this);

  srv_select_tool_ = node_.advertiseService("select_tool", &MotomanJointTrajectoryStreamer::selectToolCB, this);

  return rtn;
}

// override init() to read "robot_id" parameter and subscribe to joint_states
bool MotomanJointTrajectoryStreamer::init(SmplMsgConnection* connection, const std::vector<std::string> &joint_names,
    const std::map<std::string, double> &velocity_limits)
{
  bool rtn = true;

  ROS_INFO("MotomanJointTrajectoryStreamer: init");

  rtn &= JointTrajectoryStreamer::init(connection, joint_names, velocity_limits);

  // try to read robot_id parameter, if none specified
  if ((robot_id_ < 0))
    node_.param("robot_id", robot_id_, 0);

  rtn &= motion_ctrl_.init(connection, robot_id_);

  disabler_ = node_.advertiseService("robot_disable", &MotomanJointTrajectoryStreamer::disableRobotCB, this);

  enabler_ = node_.advertiseService("robot_enable", &MotomanJointTrajectoryStreamer::enableRobotCB, this);

  srv_select_tool_ = node_.advertiseService("select_tool", &MotomanJointTrajectoryStreamer::selectToolCB, this);

  return rtn;
}

MotomanJointTrajectoryStreamer::~MotomanJointTrajectoryStreamer()
{
  // SmplMsgConnection is not thread safe, so lock first
  // NOTE: motion_ctrl_ uses the SmplMsgConnection here
  const std::lock_guard<std::mutex> lock{smpl_msg_conx_mutex_};
  // TODO( ): Find better place to call StopTrajMode
  motion_ctrl_.setTrajMode(false);   // release TrajMode, so INFORM jobs can run
}

bool MotomanJointTrajectoryStreamer::disableRobotCB(std_srvs::Trigger::Request &req,
                                           std_srvs::Trigger::Response &res)
{
  trajectoryStop();

  {
    // SmplMsgConnection is not thread safe, so lock first
    // NOTE: motion_ctrl_ uses the SmplMsgConnection here
    const std::lock_guard<std::mutex> lock{smpl_msg_conx_mutex_};
    res.success = motion_ctrl_.setTrajMode(false);
  }

  if (!res.success)
  {
    res.message = "Motoman robot was NOT disabled. Please re-examine and retry.";
    ROS_ERROR_STREAM(res.message);
  }
  else
  {
    res.message = "Motoman robot is now disabled and will NOT accept motion commands.";
    ROS_WARN_STREAM(res.message);
  }

  return true;
}

bool MotomanJointTrajectoryStreamer::enableRobotCB(std_srvs::Trigger::Request& req, std_srvs::Trigger::Response& res)
{
  {
    // SmplMsgConnection is not thread safe, so lock first
    // NOTE: motion_ctrl_ uses the SmplMsgConnection here
    const std::lock_guard<std::mutex> lock{smpl_msg_conx_mutex_};
    res.success = motion_ctrl_.setTrajMode(true);
  }

  if (!res.success)
  {
    res.message = "Motoman robot was NOT enabled. Please re-examine and retry.";
    ROS_ERROR_STREAM(res.message);
  }
  else
  {
    res.message = "Motoman robot is now enabled and will accept motion commands.";
    ROS_WARN_STREAM(res.message);
  }

  return true;
}

bool MotomanJointTrajectoryStreamer::selectToolCB(motoman_msgs::SelectTool::Request &req,
  motoman_msgs::SelectTool::Response &res)
{
  std::string err_msg;

  {
    // SmplMsgConnection is not thread safe, so lock first
    // NOTE: motion_ctrl_ uses the SmplMsgConnection here
    const std::lock_guard<std::mutex> lock{smpl_msg_conx_mutex_};
    res.success = motion_ctrl_.selectToolFile(req.group_number, req.tool_number, err_msg);
  }

  if (!res.success)
  {
    // provide caller with failure indication
    // TODO( ): should we also return the result code?
    std::stringstream message;
    message << "Tool file change failed (grp: " << req.group_number
      << ", tool: " << req.tool_number << "): " << err_msg;
    res.message = message.str();
    ROS_ERROR_STREAM(res.message);
  }
  else
  {
    ROS_DEBUG_STREAM("Tool file changed to: " << req.tool_number
      << ", for group: " << req.group_number);
  }

  // the ROS service was successfully invoked, so return true (even if the
  // MotoROS service was not successfully invoked)
  return true;
}

// override create_message to generate JointTrajPtFull message (instead of default JointTrajPt)
bool MotomanJointTrajectoryStreamer::create_message(int seq, const trajectory_msgs::JointTrajectoryPoint& pt,
                                                    SimpleMessage* msg)
{
  JointTrajPtFull msg_data;
  JointData values;

  // copy position data
  if (!pt.positions.empty())
  {
    if (VectorToJointData(pt.positions, values))
      msg_data.setPositions(values);
    else
      ROS_ERROR_RETURN(false, "Failed to copy position data to JointTrajPtFullMessage");
  }
  else
    msg_data.clearPositions();

  // copy velocity data
  if (!pt.velocities.empty())
  {
    if (VectorToJointData(pt.velocities, values))
      msg_data.setVelocities(values);
    else
      ROS_ERROR_RETURN(false, "Failed to copy velocity data to JointTrajPtFullMessage");
  }
  else
    msg_data.clearVelocities();

  // copy acceleration data
  if (!pt.accelerations.empty())
  {
    if (VectorToJointData(pt.accelerations, values))
      msg_data.setAccelerations(values);
    else
      ROS_ERROR_RETURN(false, "Failed to copy acceleration data to JointTrajPtFullMessage");
  }
  else
    msg_data.clearAccelerations();

  // copy scalar data
  msg_data.setRobotID(robot_id_);
  msg_data.setSequence(seq);
  msg_data.setTime(pt.time_from_start.toSec());


  // convert to message
  JointTrajPtFullMessage jtpf_msg;
  jtpf_msg.init(msg_data);

  return jtpf_msg.toRequest(*msg);  // assume "request" COMM_TYPE for now
}

bool MotomanJointTrajectoryStreamer::create_message_ex(int seq, const motoman_msgs::DynamicJointPoint& point,
                                                       SimpleMessage* msg)
{
  JointTrajPtFullEx msg_data_ex;
  JointTrajPtFullExMessage jtpf_msg_ex;
  std::vector<industrial::joint_traj_pt_full::JointTrajPtFull> msg_data_vector;

  JointData values;

  int num_groups = point.num_groups;

  for (int i = 0; i < num_groups; i++)
  {
    JointTrajPtFull msg_data;

    motoman_msgs::DynamicJointsGroup pt;

    motoman_msgs::DynamicJointPoint dpoint;

    pt = point.groups[i];

    if (pt.positions.size() < 10)
    {
      int size_to_complete = 10 - pt.positions.size();

      std::vector<double> positions(size_to_complete, 0.0);
      std::vector<double> velocities(size_to_complete, 0.0);
      std::vector<double> accelerations(size_to_complete, 0.0);

      pt.positions.insert(pt.positions.end(), positions.begin(), positions.end());
      pt.velocities.insert(pt.velocities.end(), velocities.begin(), velocities.end());
      pt.accelerations.insert(pt.accelerations.end(), accelerations.begin(), accelerations.end());
    }

    // copy position data
    if (!pt.positions.empty())
    {
      if (VectorToJointData(pt.positions, values))
        msg_data.setPositions(values);
      else
        ROS_ERROR_RETURN(false, "Failed to copy position data to JointTrajPtFullMessage");
    }
    else
      msg_data.clearPositions();
    // copy velocity data
    if (!pt.velocities.empty())
    {
      if (VectorToJointData(pt.velocities, values))
        msg_data.setVelocities(values);
      else
        ROS_ERROR_RETURN(false, "Failed to copy velocity data to JointTrajPtFullMessage");
    }
    else
      msg_data.clearVelocities();

    // copy acceleration data
    if (!pt.accelerations.empty())
    {
      if (VectorToJointData(pt.accelerations, values))
        msg_data.setAccelerations(values);
      else
        ROS_ERROR_RETURN(false, "Failed to copy acceleration data to JointTrajPtFullMessage");
    }
    else
      msg_data.clearAccelerations();

    // copy scalar data
    msg_data.setRobotID(pt.group_number);
    msg_data.setSequence(seq);
    msg_data.setTime(pt.time_from_start.toSec());

    // convert to message
    msg_data_vector.push_back(msg_data);
  }

  msg_data_ex.setMultiJointTrajPtData(msg_data_vector);
  msg_data_ex.setNumGroups(num_groups);
  msg_data_ex.setSequence(seq);
  jtpf_msg_ex.init(msg_data_ex);

  return jtpf_msg_ex.toRequest(*msg);  // assume "request" COMM_TYPE for now
}

bool MotomanJointTrajectoryStreamer::create_message(int seq, const motoman_msgs::DynamicJointsGroup& pt,
                                                    SimpleMessage* msg)
{
  JointTrajPtFull msg_data;
  JointData values;
  // copy position data
  if (!pt.positions.empty())
  {
    if (VectorToJointData(pt.positions, values))
      msg_data.setPositions(values);
    else
      ROS_ERROR_RETURN(false, "Failed to copy position data to JointTrajPtFullMessage");
  }
  else
    msg_data.clearPositions();

  // copy velocity data
  if (!pt.velocities.empty())
  {
    if (VectorToJointData(pt.velocities, values))
      msg_data.setVelocities(values);
    else
      ROS_ERROR_RETURN(false, "Failed to copy velocity data to JointTrajPtFullMessage");
  }
  else
    msg_data.clearVelocities();

  // copy acceleration data
  if (!pt.accelerations.empty())
  {
    if (VectorToJointData(pt.accelerations, values))
      msg_data.setAccelerations(values);
    else
      ROS_ERROR_RETURN(false, "Failed to copy acceleration data to JointTrajPtFullMessage");
  }
  else
    msg_data.clearAccelerations();

  // copy scalar data
  msg_data.setRobotID(pt.group_number);

  msg_data.setSequence(seq);
  msg_data.setTime(pt.time_from_start.toSec());

  // convert to message
  JointTrajPtFullMessage jtpf_msg;
  jtpf_msg.init(msg_data);

  return jtpf_msg.toRequest(*msg);  // assume "request" COMM_TYPE for now
}

bool MotomanJointTrajectoryStreamer::VectorToJointData(const std::vector<double> &vec,
    JointData &joints)
{
  if (static_cast<int>(vec.size()) > joints.getMaxNumJoints())
    ROS_ERROR_RETURN(false, "Failed to copy to JointData.  Len (%d) out of range (0 to %d)",
                     (int)vec.size(), joints.getMaxNumJoints());

  joints.init();
  for (size_t i = 0; i < vec.size(); ++i)
  {
    joints.setJoint(i, vec[i]);
  }
  return true;
}

// override send_to_robot to provide controllerReady() and setTrajMode() calls
bool MotomanJointTrajectoryStreamer::send_to_robot(const std::vector<SimpleMessage>& messages)
{
  bool motion_ctrl_result = false;
  {
    // SmplMsgConnection is not thread safe, so lock first
    // NOTE: motion_ctrl_ uses the SmplMsgConnection here
    const std::lock_guard<std::mutex> lock{smpl_msg_conx_mutex_};
    motion_ctrl_result = motion_ctrl_.controllerReady();
  }

  if (!motion_ctrl_result)
    ROS_ERROR_RETURN(false, "Failed to initialize MotoRos motion, trajectory execution ABORTED. If safe, call the "
                            "'robot_enable' service to (re-)enable Motoplus motion and retry.");

  return JointTrajectoryStreamer::send_to_robot(messages);
}

// override streamingThread, to provide check/retry of MotionReply.result=BUSY
void MotomanJointTrajectoryStreamer::streamingThread()
{
  int connectRetryCount = 1;
  bool is_connected = false;
  bool is_msg_sent = false;

  ROS_INFO("Starting Motoman joint trajectory streamer thread");
  while (ros::ok())
  {
    ros::Duration(0.005).sleep();

    // automatically re-establish connection, if required
    if (connectRetryCount-- > 0)
    {
      ROS_INFO("Connecting to robot motion server");
      {
        // SmplMsgConnection is not thread safe, so lock first
        const std::lock_guard<std::mutex> lock{smpl_msg_conx_mutex_};
        this->connection_->makeConnect();
      }
      ros::Duration(0.250).sleep();  // wait for connection

      is_connected = false;
      {
        // SmplMsgConnection is not thread safe, so lock first
        // TODO(gavanderhoorn): not sure this needs to be protected by a mutex
        const std::lock_guard<std::mutex> lock{smpl_msg_conx_mutex_};
        is_connected = this->connection_->isConnected();
      }

      if (is_connected)
      {
        connectRetryCount = 0;
      }
      else if (connectRetryCount <= 0)
      {
        ROS_ERROR("Timeout connecting to robot controller.  Send new motion command to retry.");
        this->state_ = TransferStates::IDLE;
      }
      continue;
    }

    // this does not lock smpl_msg_conx_mutex_, but the mutex from JointTrajectoryStreamer
    this->mutex_.lock();

    SimpleMessage msg, tmpMsg, reply;

    switch (this->state_)
    {
    case TransferStates::IDLE:
      ros::Duration(0.250).sleep();  //  slower loop while waiting for new trajectory
      break;

    case TransferStates::STREAMING:
      if (this->current_point_ >= static_cast<int>(this->current_traj_.size()))
      {
        ROS_INFO("Trajectory streaming complete, setting state to IDLE");
        this->state_ = TransferStates::IDLE;
        break;
      }

      is_connected = false;
      {
        // SmplMsgConnection is not thread safe, so lock first
        // TODO(gavanderhoorn): not sure this needs to be protected by a mutex
        const std::lock_guard<std::mutex> lock{smpl_msg_conx_mutex_};
        is_connected = this->connection_->isConnected();
      }

      if (!is_connected)
      {
        ROS_DEBUG("Robot disconnected.  Attempting reconnect...");
        connectRetryCount = 5;
        break;
      }

      tmpMsg = this->current_traj_[this->current_point_];
      msg.init(tmpMsg.getMessageType(), CommTypes::SERVICE_REQUEST,
               ReplyTypes::INVALID, tmpMsg.getData());

      is_msg_sent = false;
      {
        // SmplMsgConnection is not thread safe, so lock first
        const std::lock_guard<std::mutex> lock{smpl_msg_conx_mutex_};
        is_msg_sent = this->connection_->sendAndReceiveMsg(msg, reply, false);
      }

      if (!is_msg_sent)
      {
        ROS_WARN("Failed sent joint point, will try again");
      }
      else
      {
        MotionReplyMessage reply_status;
        if (!reply_status.init(reply))
        {
          ROS_ERROR("Aborting trajectory: Unable to parse JointTrajectoryPoint reply");
          this->state_ = TransferStates::IDLE;
          break;
        }

        if (reply_status.reply_.getResult() == MotionReplyResults::SUCCESS)
        {
          ROS_DEBUG("Point[%d of %d] sent to controller",
                    this->current_point_, static_cast<int>(this->current_traj_.size()));
          this->current_point_++;
        }
        else if (reply_status.reply_.getResult() == MotionReplyResults::BUSY)
          break;  // silently retry sending this point
        else
        {
          ROS_ERROR_STREAM("Aborting Trajectory.  Failed to send point"
                           << " (#" << this->current_point_ << "): "
                           << MotomanMotionCtrl::getErrorString(reply_status.reply_));
          this->state_ = TransferStates::IDLE;
          break;
        }
      }
      break;

    case TransferStates::POINT_STREAMING:
      // if no points in queue, streaming complete, set to idle.
      if (this->ptstreaming_queue_.empty())
      {
        if (this->dt_ptstreaming_points_ < this->ptstreaming_timeout_)
        {
          this->dt_ptstreaming_points_ = ros::Time::now().toSec() - this->time_ptstreaming_last_point_;
          ros::Duration(0.005).sleep();
          ROS_DEBUG("Time since last point: %f", this->dt_ptstreaming_points_);
          break;
        }
        else
        {
          ROS_INFO("Point streaming complete,  setting state to IDLE");
          this->state_ = TransferStates::IDLE;
          break;
        }
      }
      // if not connected, reconnect.
      if (!this->connection_->isConnected())
      {
        ROS_DEBUG("Robot disconnected.  Attempting reconnect...");
        connectRetryCount = 5;
        break;
      }
      // otherwise, send point to robot.
      tmpMsg = this->ptstreaming_queue_.front();
      msg.init(tmpMsg.getMessageType(), CommTypes::SERVICE_REQUEST,
               ReplyTypes::INVALID, tmpMsg.getData());

      if (this->connection_->sendAndReceiveMsg(msg, reply, false))
      {
        MotionReplyMessage reply_status;
        if (!reply_status.init(reply))
        {
          ROS_ERROR("Aborting point stream operation.");
          this->state_ = TransferStates::IDLE;
          break;
        }
        if (reply_status.reply_.getResult() == MotionReplyResults::SUCCESS)
        {
          this->dt_ptstreaming_points_ = 0.0;
          this->time_ptstreaming_last_point_ = ros::Time::now().toSec();
          this->ptstreaming_queue_.pop();
          ROS_DEBUG("Point sent to controller, remaining points in queue [%lu]", this->ptstreaming_queue_.size());
        }
        else if (reply_status.reply_.getResult() == MotionReplyResults::BUSY)
        {
          ROS_DEBUG("silently resending.");
          break;  // silently retry sending this point
        }
        else
        {
          ROS_ERROR_STREAM("Aborting point stream operation.  Failed to send point"
                           << " (#" << this->current_point_ << "): "
                           << MotomanMotionCtrl::getErrorString(reply_status.reply_));
          this->state_ = TransferStates::IDLE;
          break;
        }
      }
      else
        ROS_WARN("Failed sent joint point, will try again");

      break;

    default:
      ROS_ERROR("Joint trajectory streamer: unknown state");
      this->state_ = TransferStates::IDLE;
      break;
    }
    // this does not unlock smpl_msg_conx_mutex_, but the mutex from JointTrajectoryStreamer
    this->mutex_.unlock();
  }
  ROS_WARN("Exiting trajectory streamer thread");
}

// override trajectoryStop to send MotionCtrl message
void MotomanJointTrajectoryStreamer::trajectoryStop()
{
  this->state_ = TransferStates::IDLE;  // stop sending trajectory points
  // SmplMsgConnection is not thread safe, so lock first
  // NOTE: motion_ctrl_ uses the SmplMsgConnection here
  const std::lock_guard<std::mutex> lock{smpl_msg_conx_mutex_};
  motion_ctrl_.stopTrajectory();
}

// override is_valid to include FS100-specific checks
bool MotomanJointTrajectoryStreamer::is_valid(const trajectory_msgs::JointTrajectory &traj)
{
  if (!JointTrajectoryInterface::is_valid(traj))
    return false;

  for (size_t i = 0; i < traj.points.size(); ++i)
  {
    const trajectory_msgs::JointTrajectoryPoint &pt = traj.points[i];

    // FS100 requires valid velocity data
    if (pt.velocities.empty())
      ROS_ERROR_RETURN(false, "Validation failed: Missing velocity data for trajectory pt %lu", i);
  }

  if ((cur_joint_pos_.header.stamp - ros::Time::now()).toSec() > pos_stale_time_)
    ROS_ERROR_RETURN(false, "Validation failed: Can't get current robot position.");

  // FS100 requires trajectory start at current position
  namespace IRC_utils = industrial_robot_client::utils;
  if (!IRC_utils::isWithinRange(cur_joint_pos_.name, cur_joint_pos_.position,
                                traj.joint_names, traj.points[0].positions,
                                start_pos_tol_))
  {
    ROS_ERROR_RETURN(false, "Validation failed: Trajectory doesn't start at current position.");
  }
  return true;
}

bool MotomanJointTrajectoryStreamer::is_valid(const motoman_msgs::DynamicJointTrajectory &traj)
{
  if (!JointTrajectoryInterface::is_valid(traj))
    return false;
  ros::Time time_stamp;
  int group_number;
  for (size_t i = 0; i < traj.points.size(); ++i)
  {
    for (int gr = 0; gr < traj.points[i].num_groups; gr++)
    {
      const motoman_msgs::DynamicJointsGroup &pt = traj.points[i].groups[gr];
      time_stamp = cur_joint_pos_map_[pt.group_number].header.stamp;
      // TODO( ): adjust for more joints
      group_number = pt.group_number;
      // FS100 requires valid velocity data
      if (pt.velocities.empty())
        ROS_ERROR_RETURN(false, "Validation failed: Missing velocity data for trajectory pt %lu", i);

      // FS100 requires trajectory start at current position
      namespace IRC_utils = industrial_robot_client::utils;

      if (!IRC_utils::isWithinRange(cur_joint_pos_map_[group_number].name, cur_joint_pos_map_[group_number].position,
                                    traj.joint_names, traj.points[0].groups[gr].positions,
                                    start_pos_tol_))
      {
        ROS_ERROR_RETURN(false, "Validation failed: Trajectory doesn't start at current position.");
      }
    }
  }

  if ((time_stamp - ros::Time::now()).toSec() > pos_stale_time_)
    ROS_ERROR_RETURN(false, "Validation failed: Can't get current robot position.");

  return true;
}

}  // namespace joint_trajectory_streamer
}  // namespace motoman
