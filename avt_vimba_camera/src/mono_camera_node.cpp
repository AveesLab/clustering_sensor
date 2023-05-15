/// Copyright (c) 2014,
/// Systems, Robotics and Vision Group
/// University of the Balearic Islands
/// All rights reserved.
///
/// Redistribution and use in source and binary forms, with or without
/// modification, are permitted provided that the following conditions are met:
///     * Redistributions of source code must retain the above copyright
///       notice, this list of conditions and the following disclaimer.
///     * Redistributions in binary form must reproduce the above copyright
///       notice, this list of conditions and the following disclaimer in the
///       documentation and/or other materials provided with the distribution.
///     * All advertising materials mentioning features or use of this software
///       must display the following acknowledgement:
///       This product includes software developed by
///       Systems, Robotics and Vision Group, Univ. of the Balearic Islands
///     * Neither the name of Systems, Robotics and Vision Group, University of
///       the Balearic Islands nor the names of its contributors may be used
///       to endorse or promote products derived from this software without
///       specific prior written permission.
///
/// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
/// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
/// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
/// ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
/// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
/// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
/// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
/// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
/// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
/// THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <thread>
#include <cmath>

#include <avt_vimba_camera/mono_camera_node.hpp>
#include <avt_vimba_camera_msgs/srv/load_settings.hpp>
#include <avt_vimba_camera_msgs/srv/save_settings.hpp>

// OpenCv
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/objdetect/objdetect.hpp>
#include <cv_bridge/cv_bridge.h>


using namespace std::placeholders;

namespace avt_vimba_camera
{
MonoCameraNode::MonoCameraNode() : Node("camera"), api_(this->get_logger()), cam_(std::shared_ptr<rclcpp::Node>(dynamic_cast<rclcpp::Node * >(this)))
{
  // QoS
  rclcpp::QoS system_qos = rclcpp::QoS(rclcpp::SystemDefaultsQoS());

  // Set the image publisher before streaming
  camera_info_pub_ = image_transport::create_camera_publisher(this, "~/image", rmw_qos_profile_sensor_data);
  image_publisher_ = this->create_publisher<sensor_msgs::msg::Image>("~/raw_image", system_qos);

  // Set the frame callback
  cam_.setCallback(std::bind(&avt_vimba_camera::MonoCameraNode::frameCallback, this, _1));

  start_srv_ = create_service<std_srvs::srv::Trigger>("~/start_stream", std::bind(&MonoCameraNode::startSrvCallback, this, _1, _2, _3));
  stop_srv_ = create_service<std_srvs::srv::Trigger>("~/stop_stream", std::bind(&MonoCameraNode::stopSrvCallback, this, _1, _2, _3));

  load_srv_ = create_service<avt_vimba_camera_msgs::srv::LoadSettings>("~/load_settings", std::bind(&MonoCameraNode::loadSrvCallback, this, _1, _2, _3));
  save_srv_ = create_service<avt_vimba_camera_msgs::srv::SaveSettings>("~/save_settings", std::bind(&MonoCameraNode::saveSrvCallback, this, _1, _2, _3));

  loadParams();
}

MonoCameraNode::~MonoCameraNode()
{
  cam_.stop();
  camera_info_pub_.shutdown();
}

void MonoCameraNode::loadParams()
{
  ip_ = this->declare_parameter("ip", "");
  guid_ = this->declare_parameter("guid", "");
  camera_info_url_ = this->declare_parameter("camera_info_url", "");
  frame_id_ = this->declare_parameter("frame_id", "");
  use_measurement_time_ = this->declare_parameter("use_measurement_time", false);
  ptp_offset_ = this->declare_parameter("ptp_offset", 0);
  node_index_ = this->declare_parameter("node_index", 0);
  use_image_transport_ = this->declare_parameter("use_image_transport", true);
  image_crop_ = this->declare_parameter("image_crop", false);

  RCLCPP_INFO(this->get_logger(), "Parameters loaded");
}

void MonoCameraNode::start()
{
  // Start Vimba & list all available cameras
  api_.start();

  // Start camera
  cam_.start(ip_, guid_, frame_id_, camera_info_url_);
  cam_.startImaging();
}

void MonoCameraNode::frameCallback(const FramePtr& vimba_frame_ptr)
{
  rclcpp::Time ros_time = this->get_clock()->now();

  // getNumSubscribers() is not yet supported in Foxy, will be supported in later versions

  sensor_msgs::msg::Image img;
  if (api_.frameToImage(vimba_frame_ptr, img))
  {
    if (use_measurement_time_)
    {
      img.header.frame_id = std::to_string(this->node_index_);

      VmbUint64_t frame_timestamp;
      vimba_frame_ptr->GetTimestamp(frame_timestamp);
      img.header.stamp = rclcpp::Time(cam_.getTimestampRealTime(frame_timestamp) * 1.0e+9) + rclcpp::Duration(ptp_offset_, 0);

      RCLCPP_INFO(this->get_logger(), "capture time : %lf sec.", cam_.getTimestampRealTime(frame_timestamp) + rclcpp::Duration(ptp_offset_, 0).seconds());
    }
    else
    {
      img.header.stamp = ros_time;
    }

    if (image_crop_)
    {
      try
      {
        cv_bridge::CvImagePtr cv_ptr = cv_bridge::toCvCopy(img, img.encoding);
        cv::Mat color_image;
        cv::Mat resized_image;

        cv::cvtColor(cv_ptr->image, color_image, cv::COLOR_BayerRG2RGB);
        cv::resize(color_image, resized_image, cv::Size(640,480));

        cv_bridge::CvImage cv_bridge = cv_bridge::CvImage(img.header, sensor_msgs::image_encodings::BGR8, resized_image);
        img = *(cv_bridge.toImageMsg());
      }
      catch (cv::Exception & e)
      {
        RCLCPP_INFO(this->get_logger(), "cv_bridge exception: %s", e.what());
      }
    }

    if (use_image_transport_)
    {
      // Note: getCameraInfo() doesn't fill in header frame_id or stamp
      sensor_msgs::msg::CameraInfo::SharedPtr ci = std::make_shared<sensor_msgs::msg::CameraInfo>(cam_.getCameraInfo());
      
      ci->header.frame_id = img.header.frame_id;
      ci->header.stamp = img.header.stamp;
      ci->height = 480;
      ci->width = 640;

      camera_info_pub_.publish(img, *ci);
      RCLCPP_INFO(this->get_logger(), "Publish image_transport image message");
    }
    else
    {
      image_publisher_->publish(img);
      RCLCPP_INFO(this->get_logger(), "Publish sensor_msgs image message");
    }

    VmbUint64_t frame_ID;
    vimba_frame_ptr->GetFrameID(frame_ID);
    RCLCPP_INFO(this->get_logger(), "Frame ID : %d .", frame_ID);
  }
  else
  {
    RCLCPP_WARN_STREAM(this->get_logger(), "Function frameToImage returned 0. No image published.");
  }

}

void MonoCameraNode::startSrvCallback(const std::shared_ptr<rmw_request_id_t> request_header,
                                      const std_srvs::srv::Trigger::Request::SharedPtr req,
                                      std_srvs::srv::Trigger::Response::SharedPtr res) {
  (void)request_header;
  (void)req;

  cam_.startImaging();
  cam_.setForceStop(false);
  auto state = cam_.getCameraState();
  res->success = state != CameraState::ERROR;
}

void MonoCameraNode::stopSrvCallback(const std::shared_ptr<rmw_request_id_t> request_header,
                                     const std_srvs::srv::Trigger::Request::SharedPtr req,
                                     std_srvs::srv::Trigger::Response::SharedPtr res)
{
  (void)request_header;
  (void)req;

  cam_.stopImaging();
  cam_.setForceStop(true);
  auto state = cam_.getCameraState();
  res->success = state != CameraState::ERROR;
}

void MonoCameraNode::loadSrvCallback(const std::shared_ptr<rmw_request_id_t> request_header,
                                     const avt_vimba_camera_msgs::srv::LoadSettings::Request::SharedPtr req,
                                     avt_vimba_camera_msgs::srv::LoadSettings::Response::SharedPtr res) 
{
  (void)request_header;
  auto extension = req->input_path.substr(req->input_path.find_last_of(".") + 1);
  if (extension != "xml")
  {
    RCLCPP_WARN(this->get_logger(), "Invalid file extension. Only .xml is supported.");
    res->result = false;
  } 
  else 
  {
    res->result = cam_.loadCameraSettings(req->input_path);
  }
}

void MonoCameraNode::saveSrvCallback(const std::shared_ptr<rmw_request_id_t> request_header,
                                     const avt_vimba_camera_msgs::srv::SaveSettings::Request::SharedPtr req,
                                     avt_vimba_camera_msgs::srv::SaveSettings::Response::SharedPtr res) 
{
  (void)request_header;
  auto extension = req->output_path.substr(req->output_path.find_last_of(".") + 1);
  if (extension != "xml")
  {
    RCLCPP_WARN(this->get_logger(), "Invalid file extension. Only .xml is supported.");
    res->result = false;
  } 
  else 
  {
    res->result = cam_.saveCameraSettings(req->output_path);
  }
}
}  // namespace avt_vimba_camera
