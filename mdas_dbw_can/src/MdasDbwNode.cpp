#include "MdasDbwNode.h"

namespace mdas_dbw_can
{
    DbwNode::DbwNode(ros::NodeHandle &node, ros::NodeHandle &priv_nh) {
        std::string throttle_id_str, brake_id_str, steer_id_str, feedback_id_str;


        priv_nh.param<std::string>("throttle_can_id", throttle_id_str, "0x1ADB0000");
        priv_nh.param<std::string>("brake_can_id", brake_id_str, "0x18DB0000");
        priv_nh.param<std::string>("steering_can_id", steer_id_str, "0x19DB0000");
        priv_nh.param<std::string>("feedback_can_id", feedback_id_str, "0x1CDBFFFF");

        // Convert hex strings to ints
        throttle_id = std::stoi(throttle_id_str, nullptr, 16);
        brake_id = std::stoi(brake_id_str, nullptr, 16);
        steer_id = std::stoi(steer_id_str, nullptr, 16);
        throttlebrake_feedback_id = std::stoi(feedback_id_str, nullptr, 16);

        throttlePub = node.advertise<mdas_dbw_msgs::ThrottleReport>("mdas_dbw/feedback/throttle", 10);
        brakePub = node.advertise<mdas_dbw_msgs::BrakeReport>("mdas_dbw/feedback/brake", 10);
        steeringPub = node.advertise<mdas_dbw_msgs::SteeringReport>("mdas_dbw/feedback/steering", 100);
        canSender = node.advertise<can_msgs::Frame>("sent_messages", 30);

        const ros::TransportHints NODELAY = ros::TransportHints().tcpNoDelay();
        feedbackSub = node.subscribe("received_messages", 100, &DbwNode::ReceivedMessagesCallback, this, NODELAY);
        brakeSub = node.subscribe("mdas_dbw/cmd/brake", 1, &DbwNode::ReceivedBrakeCmdCallback, this, NODELAY);
        throttleSub = node.subscribe("mdas_dbw/cmd/throttle", 1, &DbwNode::ReceivedThrottleCmdCallback, this, NODELAY);
        steeringSub = node.subscribe("mdas_dbw/cmd/steering", 1, &DbwNode::ReceivedSteerCmdCallback, this, NODELAY);


        timer = node.createTimer(ros::Duration(0.1), &DbwNode::CanSend, this);
    }

    DbwNode::~DbwNode() {
    }

    inline can_msgs::Frame DbwNode::BuildBrakeMsg(std::uint8_t pedal_cmd) {
        can_msgs::Frame msg;

        msg.is_extended = true;
        msg.is_error = false;
        msg.is_rtr = false;
        msg.data[0] = CONTROL_SRC_ID;
        msg.data[1] = 0;
        msg.data[2] = 0;
        msg.dlc = 4;

        msg.id = brake_id;
        msg.data[3] = pedal_cmd;
        msg.header.stamp = ros::Time::now();

        return msg;
    }

    inline can_msgs::Frame DbwNode::BuildThrottleMsg(std::uint8_t pedal_cmd) {
        can_msgs::Frame msg;

        msg.is_extended = true;
        msg.is_error = false;
        msg.is_rtr = false;
        msg.data[0] = CONTROL_SRC_ID;
        msg.data[1] = 0;
        msg.data[2] = 0;
        msg.dlc = 4;

        msg.id = throttle_id;
        msg.data[3] = pedal_cmd;

        msg.header.stamp = ros::Time::now();

        return msg;
    }

    inline can_msgs::Frame DbwNode::BuildSteeringMsg(std::float_t wheel_cmd) {
        can_msgs::Frame msg;

        msg.is_extended = true;
        msg.is_error = false;
        msg.is_rtr = false;
        msg.data[0] = 0x01; //TODO: Temporary until finalized with Naso
        msg.data[1] = 0;
        msg.data[2] = 0;
        msg.dlc = 4;

        msg.id = steer_id;

        // Translate steering angle to command
        float steer = STEER_MULT * wheel_cmd + STEER_ADD;
        std::uint16_t steerInt = (uint16_t)steer;

        // Build command message
        msg.data[2] = (steerInt >> 8) & 0xff;
        msg.data[3] = steerInt & 0xff;

        msg.header.stamp = ros::Time::now();

        return msg;
    }

    inline mdas_dbw_msgs::BrakeReport DbwNode::BuildBrakeReport(const can_msgs::Frame::ConstPtr& canFrame) {
        mdas_dbw_msgs::BrakeReport msg;

        msg.enabled = brakeCmd.enable;
        msg.pedal_input = canFrame->data[2];
        msg.override = 0;
        msg.header.stamp = ros::Time::now();

        if(msg.pedal_input > msg.BRAKE_MAX) {
            ROS_WARN("Brake input greater than expected: %d%%", msg.pedal_input);
        }

        return msg;
    }

    inline mdas_dbw_msgs::ThrottleReport DbwNode::BuildThrottleReport(const can_msgs::Frame::ConstPtr& canFrame) {
        mdas_dbw_msgs::ThrottleReport msg;

        msg.enabled = throttleCmd.enable;
        msg.pedal_input = canFrame->data[2];
        msg.override = 0;
        msg.header.stamp = ros::Time::now();

        if(msg.pedal_input > msg.THROTTLE_MAX) {
            ROS_WARN("Brake input greater than expected: %d%%", msg.pedal_input);
        }

        return msg;
    }

    inline mdas_dbw_msgs::SteeringReport DbwNode::BuildSteeringReport(const can_msgs::Frame::ConstPtr& canFrame) {
        mdas_dbw_msgs::SteeringReport msg;

        msg.enabled = steerCmd.enable;

        msg.steering_wheel_angle = 0; // TODO: Unused, talk to Naso to resolve this
        msg.steering_wheel_angle_velocity = 0; // TODO: Unused, talk to Naso to resolve this
        msg.steering_wheel_torque = 0; // TODO: Unused, talk to Naso to resolve this
        msg.override = 0;

        msg.header.stamp = ros::Time::now();

        return msg;
    }

    void DbwNode::TimeoutDisable() {
        throttleTimeout++;
        brakeTimeout++;
        steerTimeout++;

        if(throttleTimeout == TIMEOUT_INTERVAL) {
            throttleCmd.enable = false;
            ROS_WARN("Throttle command timeout, disabling");
        }
        if(brakeTimeout == TIMEOUT_INTERVAL) {
            brakeCmd.enable = false;
            ROS_WARN("Brake command timeout, disabling");
        }
        if(steerTimeout == TIMEOUT_INTERVAL ) {
            steerCmd.enable = false;
            ROS_WARN("Steering command timeout, disabling");
        }
    }

    void DbwNode::ReceivedMessagesCallback(const can_msgs::Frame::ConstPtr& canFrame) {
        if((canFrame->id != throttlebrake_feedback_id) || (canFrame->id != STEERING_POSITION_CAN_ID) ||
           (canFrame->id != STEERING_VELOCITY_CAN_ID) || (canFrame->id != STEERING_TORQUE_CAN_ID)) {
            return;
        }
        std::uint8_t srcId = canFrame->data[0];

        switch(srcId) {
            case BRAKE_FEEDBACK_SRC_ID:
            {
                mdas_dbw_msgs::BrakeReport msg = BuildBrakeReport(canFrame);
                brakePub.publish(msg);
                return;
            }
            case THROTTLE_FEEDBACK_SRC_ID:
            {
                mdas_dbw_msgs::ThrottleReport msg = BuildThrottleReport(canFrame);
                throttlePub.publish(msg);
                return;
            }
            case STEERING_FEEDBACK_SRC_ID:
            {
                mdas_dbw_msgs::SteeringReport msg = BuildSteeringReport(canFrame);
                steeringPub.publish(msg);
                return;
            }
            default:
            {
                ROS_ERROR("Unknown Feedback Source ID: %x", srcId);
                return;
            }
        };
    }

    void DbwNode::ReceivedBrakeCmdCallback(const mdas_dbw_msgs::BrakeCmd& brakeMsg) {
        brakeCmd = brakeMsg;

        if(brakeCmd.pedal_cmd > brakeCmd.BRAKE_MAX) { // Clamp input
            brakeCmd.pedal_cmd = brakeCmd.BRAKE_MAX;
            ROS_WARN("Brake Command >%d%%, input clamped", brakeCmd.BRAKE_MAX);
        }

        brakeTimeout = 0; // Received message, reset timeout
    }

    void DbwNode::ReceivedSteerCmdCallback(const mdas_dbw_msgs::SteeringCmd& steerMsg) {
        steerCmd = steerMsg;
        const std::int32_t steerNeg = steerCmd.ANGLE_MAX * -1;

        if(steerCmd.steering_wheel_angle_cmd > steerCmd.ANGLE_MAX) { // Clamp input
            steerCmd.steering_wheel_angle_cmd = steerCmd.ANGLE_MAX;
            ROS_WARN("Steering command >%d degrees, input clamped", steerCmd.ANGLE_MAX);
        } else if(steerCmd.steering_wheel_angle_cmd < steerNeg) {
            steerCmd.steering_wheel_angle_cmd = steerNeg;
            ROS_WARN("Steering command <%d degrees, input clamped", steerNeg);
        }

        steerTimeout = 0; // Received emssage, reset timeout
    }

    void DbwNode::ReceivedThrottleCmdCallback(const mdas_dbw_msgs::ThrottleCmd& throttleMsg) {
        throttleCmd = throttleMsg;

        if(throttleCmd.pedal_cmd > throttleCmd.THROTTLE_MAX) { // Clamp input
            throttleCmd.pedal_cmd = throttleCmd.THROTTLE_MAX;
            ROS_WARN("Steering command >%d%%, input clamped", throttleCmd.THROTTLE_MAX);
        }

        throttleTimeout = 0; // Received message, reset timeout
    }

    void DbwNode::CanSend(const ros::TimerEvent&) {
        can_msgs::Frame msg;

        if(brakeCmd.enable) {
            msg = BuildBrakeMsg(brakeCmd.pedal_cmd);
            canSender.publish(msg);
        } else {
            msg = BuildBrakeMsg(0);
            canSender.publish(msg);
        }

        if(throttleCmd.enable) {
            msg = BuildThrottleMsg(throttleCmd.pedal_cmd);
            canSender.publish(msg);
        } else {
            msg = BuildThrottleMsg(0);
            canSender.publish(msg);
        }

        if(steerCmd.enable) {
            msg = BuildSteeringMsg(steerCmd.steering_wheel_angle_cmd);
            canSender.publish(msg);
        } else {
            msg = BuildSteeringMsg(0);
            canSender.publish(msg);
        }

        TimeoutDisable();
    }
}