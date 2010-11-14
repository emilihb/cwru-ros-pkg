#include <math.h>
#include <angles/angles.h>
#include <algorithm>
#include <wagon_handle_steering/wagon_handle_steering.h>
#include <pluginlib/class_list_macros.h>

PLUGINLIB_DECLARE_CLASS(wagon_handle_steering, WagonHandleSteering, wagon_handle_steering::WagonHandleSteering, nav_core::BaseLocalPlanner)

namespace wagon_handle_steering {
  WagonHandleSteering::WagonHandleSteering(): tf_(NULL), costmap_ros_(NULL) {}

  void WagonHandleSteering::initialize(std::string name, tf::TransformListener* tf, costmap_2d::Costmap2DROS* costmap_ros){
    tf_ = tf;
    costmap_ros_ = costmap_ros;
    current_waypoint_ = 0;
    started_reorienting_ = false;
    goal_reached_time_ = ros::Time::now();
    ros::NodeHandle node_private("~/" + name);

    collision_planner_.initialize(name, tf_, costmap_ros_);

    node_private.param("handle_length", handle_length_, 1.0);
    node_private.param("reorient_dist", reorient_dist_, .25);
    node_private.param("rotate_in_place_heading", rotate_in_place_head_, 0.2);
    node_private.param("rotate_in_place_distance", rotate_in_place_dist_, 0.1);
    node_private.param("desired_speed", desired_speed_, 0.5);

    node_private.param("tolerance_trans", tolerance_trans_, 0.02);
    node_private.param("tolerance_rot", tolerance_rot_, 0.04);
    node_private.param("tolerance_timeout", tolerance_timeout_, 0.5);

    node_private.param("max_vel_lin", max_vel_lin_, 0.9);
    node_private.param("max_vel_th", max_vel_th_, 1.4);

    node_private.param("min_vel_lin", min_vel_lin_, 0.1);
    node_private.param("min_vel_th", min_vel_th_, 0.0);
    node_private.param("min_in_place_vel_th", min_in_place_vel_th_, 0.0);
    node_private.param("in_place_trans_vel", in_place_trans_vel_, 0.0);

    node_private.param("trans_stopped_velocity", trans_stopped_velocity_, 1e-4);
    node_private.param("rot_stopped_velocity", rot_stopped_velocity_, 1e-4);

    ros::NodeHandle node;
    odom_sub_ = node.subscribe<nav_msgs::Odometry>("odom", 1, boost::bind(&WagonHandleSteering::odomCallback, this, _1));
    vel_pub_ = node.advertise<geometry_msgs::Twist>("cmd_vel", 10);

    ROS_DEBUG("Initialized");
  }

  void WagonHandleSteering::odomCallback(const nav_msgs::Odometry::ConstPtr& msg){
    //we assume that the odometry is published in the frame of the base
    boost::mutex::scoped_lock lock(odom_lock_);
    base_odom_.twist.twist.linear.x = msg->twist.twist.linear.x;
    base_odom_.twist.twist.linear.y = msg->twist.twist.linear.y;
    base_odom_.twist.twist.angular.z = msg->twist.twist.angular.z;
    ROS_DEBUG("In the odometry callback with velocity values: (%.2f, %.2f, %.2f)",
        base_odom_.twist.twist.linear.x, base_odom_.twist.twist.linear.y, base_odom_.twist.twist.angular.z);
  }

  bool WagonHandleSteering::stopped(){
    //copy over the odometry information
    nav_msgs::Odometry base_odom;
    {
      boost::mutex::scoped_lock lock(odom_lock_);
      base_odom = base_odom_;
    }

    return fabs(base_odom.twist.twist.angular.z) <= rot_stopped_velocity_
      && fabs(base_odom.twist.twist.linear.x) <= trans_stopped_velocity_
      && fabs(base_odom.twist.twist.linear.y) <= trans_stopped_velocity_;
  }


  bool WagonHandleSteering::intersectedWithCircle(const tf::Point& start_p, const tf::Point& robot_p, const tf::Vector3& direction, tf::Point& intersectionPoint) {
	  tf::Vector3 normalized_direction = direction.normalized();
	  tf::Vector3 v = start_p - robot_p;
	  double temp2 = v.dot(normalized_direction);
	  temp2 *= temp2;
	  double temp3 = v.dot(v) - (handle_length_ * handle_length_);
	  double discriminant = temp2 - temp3;
	  if (discriminant < 0.0) {
	  	return false;
	  } else {
	  	temp2 = -1.0 * v.dot(normalized_direction);	
		temp3 = sqrt(discriminant);
		double t, t1, t2;
		t1 = temp2 + temp3;
		t2 = temp2 - temp3;
		t = std::max(t1, t2);

		if (t < 0.0) {
			if((t1 < 0.0) && (t2 < 0.0)) {
				return false;
			} else if ((t1 < 0.0) && (t2 >= 0.0)) {
				t = t2;
			} else if ((t1 >= 0.0) && (t2 < 0.0)) {
				t = t1;
			}
		}
		intersectionPoint = start_p + t * normalized_direction;
		return true;
	  }
  }

  bool WagonHandleSteering::shouldRotateInPlace(const tf::Point& start, const tf::Point& end, const tf::Pose& current_loc) {
 	tf::Vector3 segment1 = end - start; 
	tf::Vector3 segment2 = start - current_loc.getOrigin();
	
	double term1 = segment1.getX() * segment2.getY();
	double term2 = segment1.getY() * segment2.getX();

	double denominator = sqrt(pow(segment1.getX(), 2.0) - pow(segment1.getY(), 2.0));

	double numerator = term1 - term2;
	double distance = fabs(numerator) / denominator;
	
	if (distance < rotate_in_place_dist_) {
		double line_heading = -1.0 * atan2(segment1.getY(), segment1.getX());
		double min_angle = angles::shortest_angular_distance(tf::getYaw(current_loc.getRotation()), line_heading);
		if (fabs(min_angle) > rotate_in_place_head_) {
			return true;
		} else {
			return false;
		}
	} else {
		return false;
	}
  }

  bool WagonHandleSteering::computeVelocityCommands(geometry_msgs::Twist& cmd_vel){
    //get the current pose of the robot in the fixed frame
    double heading = 0.0;
    double speed = 0.0;
    tf::Stamped<tf::Pose> robot_pose;
    if(!costmap_ros_->getRobotPose(robot_pose)){
      ROS_ERROR("Can't get robot pose");
      geometry_msgs::Twist empty_twist;
      cmd_vel = empty_twist;
      return false;
    }

    //we want to compute a velocity command based on our current waypoint
    tf::Stamped<tf::Pose> target_pose, last_target_pose;
    tf::poseStampedMsgToTF(global_plan_[current_waypoint_], target_pose);
    tf::poseStampedMsgToTF(global_plan_[current_waypoint_ - 1], last_target_pose);

    ROS_DEBUG("WagonHandleSteering: current robot pose %f %f ==> %f", robot_pose.getOrigin().x(), robot_pose.getOrigin().y(), tf::getYaw(robot_pose.getRotation()));
    ROS_DEBUG("WagonHandleSteering: target robot pose %f %f ==> %f", target_pose.getOrigin().x(), target_pose.getOrigin().y(), tf::getYaw(target_pose.getRotation()));

    tf::Point robot_p = robot_pose.getOrigin();
    tf::Point target_p = target_pose.getOrigin();
    tf::Point last_target_p = last_target_pose.getOrigin();

    //get the distance between the robot and the end point of the path segment
    double distance = target_p.distance(robot_p);

    if (distance < handle_length_) {
    	if ((distance < reorient_dist_) || started_reorienting_) {
		started_reorienting_ = true;
		heading = tf::getYaw(target_pose.getRotation());
		ROS_DEBUG("WagonHandleSteering: Reorienting to the desired heading");
		speed = 0.0;
	} else if (!started_reorienting_) {
		tf::Vector3 diff = target_p - robot_p;
		heading = -1.0 * atan2(diff.getY(), diff.getX());
		ROS_DEBUG("WagonHandleSteering: Heading directly towards the goal"); 
		speed = exp(-1.0 * (handle_length_ / distance)) * desired_speed_;
	}
	ROS_DEBUG("WagonHandleSteering: Goal is within the handle length. Heading directly towards the goal on a heading of %f", heading);
    } else {
	tf::Point intersection_p;
    	started_reorienting_ = false;
	tf::Vector3 current_segment = target_p - last_target_p;
	bool intersected = intersectedWithCircle(last_target_p, robot_p, current_segment, intersection_p);	
	ROS_DEBUG("WagonHandleSteering: Intersection point was x: %f, y: %f", intersection_p.getX(), intersection_p.getY());
	if(intersected) {
		tf::Vector3 diff = intersection_p - robot_p;
		heading =  -1.0 * atan2(diff.getY(), diff.getX());
		speed = desired_speed_;
		ROS_DEBUG("Intersected with wagon handle radius");
	} else {
		ROS_WARN("No intersection found");
	       	geometry_msgs::Twist empty_twist;
      		cmd_vel = empty_twist;
      		return false;
	}
    }

    bool rotate_only = shouldRotateInPlace(last_target_p, target_p, robot_pose);
    if (rotate_only) {
	ROS_DEBUG("Rotating in place only");
    	speed = 0.0;
    }
    geometry_msgs::Twist limit_vel = limitTwist(heading, speed);

    bool legal_traj = collision_planner_.checkTrajectory(limit_vel.linear.x, limit_vel.linear.y, limit_vel.angular.z, true);

    if(!legal_traj){
      ROS_ERROR("Not legal (%.2f, %.2f, %.2f)", limit_vel.linear.x, limit_vel.linear.y, limit_vel.angular.z);
      geometry_msgs::Twist empty_twist;
      cmd_vel = empty_twist;
      return false;
    }

    //if it is legal... we'll pass it on
    cmd_vel = limit_vel;

    /*
    //if we haven't reached our goal... we'll update time
    if (fabs(diff.linear.x) > tolerance_trans_ || fabs(diff.linear.y) > tolerance_trans_ || fabs(diff.angular.z) > tolerance_rot_)
      goal_reached_time_ = ros::Time::now();
    else if(current_waypoint_ < (global_plan_.size() - 1)){
      //if we're not on the last waypoint... but we've reached the next waypoint in the plan... we'll update our current waypoint
      goal_reached_time_ = ros::Time::now();
      current_waypoint_++;
    }
    //check if we've reached our goal for long enough to succeed
    else if(goal_reached_time_ + ros::Duration(tolerance_timeout_) < ros::Time::now()){
      geometry_msgs::Twist empty_twist;
      cmd_vel = empty_twist;
    } */

    return true;
  }

  bool WagonHandleSteering::setPlan(const std::vector<geometry_msgs::PoseStamped>& global_plan){
    current_waypoint_ = 0;
    goal_reached_time_ = ros::Time::now();
    if(!transformGlobalPlan(*tf_, global_plan, *costmap_ros_, costmap_ros_->getGlobalFrameID(), global_plan_)){
      ROS_ERROR("Could not transform the global plan to the frame of the controller");
      return false;
    }
    return true;
  }

  bool WagonHandleSteering::isGoalReached(){
    if(goal_reached_time_ + ros::Duration(tolerance_timeout_) < ros::Time::now() && stopped()){
      return true;
    }
    return false;
  }

  geometry_msgs::Twist WagonHandleSteering::limitTwist(const double desired_heading, const double desired_speed)
  {
	geometry_msgs::Twist res;
    return res;
  }

  bool WagonHandleSteering::transformGlobalPlan(const tf::TransformListener& tf, const std::vector<geometry_msgs::PoseStamped>& global_plan, 
      const costmap_2d::Costmap2DROS& costmap, const std::string& global_frame,
      std::vector<geometry_msgs::PoseStamped>& transformed_plan){
    const geometry_msgs::PoseStamped& plan_pose = global_plan[0];

    transformed_plan.clear();

    try{
      if (!global_plan.size() > 0)
      {
        ROS_ERROR("Recieved plan with zero length");
        return false;
      }

      tf::StampedTransform transform;
      tf.lookupTransform(global_frame, ros::Time(), 
          plan_pose.header.frame_id, plan_pose.header.stamp, 
          plan_pose.header.frame_id, transform);

      tf::Stamped<tf::Pose> tf_pose;
      geometry_msgs::PoseStamped newer_pose;
      //now we'll transform until points are outside of our distance threshold
      for(unsigned int i = 0; i < global_plan.size(); ++i){
        const geometry_msgs::PoseStamped& pose = global_plan[i];
        poseStampedMsgToTF(pose, tf_pose);
        tf_pose.setData(transform * tf_pose);
        tf_pose.stamp_ = transform.stamp_;
        tf_pose.frame_id_ = global_frame;
        poseStampedTFToMsg(tf_pose, newer_pose);

        transformed_plan.push_back(newer_pose);
      }
    }
    catch(tf::LookupException& ex) {
      ROS_ERROR("No Transform available Error: %s\n", ex.what());
      return false;
    }
    catch(tf::ConnectivityException& ex) {
      ROS_ERROR("Connectivity Error: %s\n", ex.what());
      return false;
    }
    catch(tf::ExtrapolationException& ex) {
      ROS_ERROR("Extrapolation Error: %s\n", ex.what());
      if (global_plan.size() > 0)
        ROS_ERROR("Global Frame: %s Plan Frame size %d: %s\n", global_frame.c_str(), (unsigned int)global_plan.size(), global_plan[0].header.frame_id.c_str());

      return false;
    }

    return true;
  }
};