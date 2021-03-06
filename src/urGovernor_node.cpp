#include <ros/ros.h>

// Shared lib
#include "SerialPacket.h"

// For kinematics
#include "deltaRobot.h"

// Srv and msg types
#include <urGovernor/FetchWeed.h>
#include <urGovernor/MarkUprooted.h>
#include <urGovernor/RemoveWeed.h>

#include <urVision/weedDataArray.h>
#include <urGovernor/SerialWrite.h>
#include <urGovernor/SerialRead.h>
#include <geometry_msgs/Point.h>
#include <geometry_msgs/Vector3.h>

// Parameters to read from configs
std::string fetchWeedServiceName;
std::string markUprootedServiceName;
std::string rmWeedServiceName;

float overallRate;

std::string serialServiceWriteName;
std::string serialServiceReadName;
std::string velocityPublisherName;

int restAngle1, restAngle2, restAngle3;
float cartesianLimitXMax, cartesianLimitXMin, cartesianLimitYMax, cartesianLimitYMin;
float angleLimit;

float initSleepTime;
float actuationTimeOverride;
int minUpdateAngle;
int maxUpdateAngle;

const int relativeAngleFlag = false;

float toolOffset;
float soilOffset;
float targetYGain;
float curYVel;

// Time to actuate end-effector
double endEffectorTime = 0;
bool endEffectorRunning = true;
bool armDown = false;
float stayDownDist = 0;

// Connections to Serial interface services
ros::ServiceClient serialWriteClient;
ros::ServiceClient serialReadClient;
ros::ServiceClient fetchWeedClient;
ros::ServiceClient markUprootedClient;
ros::ServiceClient rmWeedClient;

const int logFetchWeedInterval = 5;

int serialTimeoutMs;
int commandTimeoutSec;
int motorSpeedDegS;
int motorAccelDegSS;

// General parameters for this node
bool readGeneralParameters(ros::NodeHandle nodeHandle)
{
    if (!nodeHandle.getParam("fetch_weed_service", fetchWeedServiceName)) return false;
    if (!nodeHandle.getParam("mark_uprooted_service", markUprootedServiceName)) return false;
    if (!nodeHandle.getParam("remove_weed_service", rmWeedServiceName)) return false;

    if (!nodeHandle.getParam("velocity_publisher", velocityPublisherName)) return false;
   
    if (!nodeHandle.getParam("controller_overall_rate", overallRate)) return false;
    if (!nodeHandle.getParam("init_sleep_time", initSleepTime)) return false;
    if (!nodeHandle.getParam("max_actuation_time_override", actuationTimeOverride)) return false;
    if (!nodeHandle.getParam("min_update_angle", minUpdateAngle)) return false;
    if (!nodeHandle.getParam("max_update_angle", maxUpdateAngle)) return false;

    if (!nodeHandle.getParam("rest_angle_1", restAngle1)) return false;
    if (!nodeHandle.getParam("rest_angle_2", restAngle2)) return false;
    if (!nodeHandle.getParam("rest_angle_3", restAngle3)) return false;

    if (!nodeHandle.getParam("cartesian_limit_x_max", cartesianLimitXMax)) return false;
    if (!nodeHandle.getParam("cartesian_limit_x_min", cartesianLimitXMin)) return false;
    if (!nodeHandle.getParam("cartesian_limit_y_max", cartesianLimitYMax)) return false;
    if (!nodeHandle.getParam("cartesian_limit_y_min", cartesianLimitYMin)) return false;

    if (!nodeHandle.getParam("angle_limit", angleLimit)) return false;

    if (!nodeHandle.getParam("end_effector_time_s", endEffectorTime)) return false;
    if (!nodeHandle.getParam("stay_down_dist_cm", stayDownDist)) return false;
   
    if (!nodeHandle.getParam("tool_offset", toolOffset)) return false;
    if (!nodeHandle.getParam("soil_offset", soilOffset)) return false;
    if (!nodeHandle.getParam("target_y_gain", targetYGain)) return false;

    if (!nodeHandle.getParam("serial_output_service", serialServiceWriteName)) return false;
    if (!nodeHandle.getParam("serial_input_service", serialServiceReadName)) return false;

    if (!nodeHandle.getParam("serial_timeout_ms", serialTimeoutMs)) return false;
    if (!nodeHandle.getParam("command_timeout_sec", commandTimeoutSec)) return false;

    if (!nodeHandle.getParam("motor_speed_deg_s", motorSpeedDegS)) return false;
    if (!nodeHandle.getParam("motor_accel_deg_s_s", motorAccelDegSS)) return false;
   
    return true;
}

// Send CmdMsg over serial
bool sendCmd(SerialUtils::CmdMsg msg)
{
    urGovernor::SerialWrite serialWrite;
    // Pack message
    std::vector<char> buff;
    SerialUtils::pack(buff, msg);

    serialWrite.request.command = std::string(buff.begin(), buff.end());

    // Send angles to HAL (via calling the serial WRITE client)
    return serialWriteClient.call(serialWrite);
}

// Check for callback from motors
bool checkSuccess(SerialUtils::CmdMsg exp_msg)
{
    urGovernor::SerialRead serialRead;   
    SerialUtils::CmdMsg msg;

    // Try to read on serial
    if (serialReadClient.call(serialRead))
    {
        std::vector<char> v(serialRead.response.command.begin(), serialRead.response.command.end());
        
        msg.cmd_success = 0;
        // Unpack response from read
        SerialUtils::unpack(v, msg);

        // Check if we are done
        if (msg == exp_msg && msg.cmd_success)
        {
            return true;
        }
    }

    return false;
}

// Wait for success
bool waitSuccess(SerialUtils::CmdMsg exp_msg)
{
    urGovernor::SerialRead serialRead;
    SerialUtils::CmdMsg msg;

    ros::Rate loopRate( 1.0 / (serialTimeoutMs / 1000.0));
    ros::WallTime start_time = ros::WallTime::now();
    double timeout = commandTimeoutSec;
    while (ros::ok() && (ros::WallTime::now()- start_time).toSec() < timeout)
    {
        // Wait for arm done
        // This is done by calling the serial READ client
        // This should block until we get a CmdMsg FROM the serial line
        if (checkSuccess(exp_msg)) {
            ROS_DEBUG("Teensy callback received.");
            return true;
        }

        if (serialReadClient.call(serialRead))
        {
            std::vector<char> v(serialRead.response.command.begin(), serialRead.response.command.end());
            
            msg.cmd_success = 0;
            // Unpack response from read
            SerialUtils::unpack(v, msg);

            // This should indicate that we are done
            if (msg == exp_msg && msg.cmd_success)
            {
                ROS_DEBUG("Teensy callback received.");
                return true;
            }
        }
        else
        {
            ROS_DEBUG("No response from Teensy ... retrying ...");
        }
        loopRate.sleep();
    }

    ROS_ERROR("Timed out waiting for response from Teensy");
    return false;
}

// Configure speed and acceleration in degrees/second -- value of 0 is discarded
bool configMotors(int speedDegS, int accelDegSS)
{
    SerialUtils::CmdMsg msg = { .cmd_type = SerialUtils::CMDTYPE_CONFIG };
    msg.mtr_speed_deg_s = speedDegS;
    msg.mtr_accel_deg_s_s = accelDegSS;
    sendCmd(msg);
    if(!waitSuccess(msg)) {
        ROS_ERROR("Unable to configure motors");
        return false;
    }
    return true;
}

// Single set point, updates only, returns immediately
bool sendArmAngles(int angle1Deg, int angle2Deg, int angle3Deg, SerialUtils::CmdMsg* p_msg = NULL)
{
    if (angle1Deg == 10)
        angle1Deg = 11;
    if (angle2Deg == 10)
        angle2Deg = 11;
    if (angle3Deg == 10)
        angle3Deg = 11;

    if (angle1Deg < restAngle1 &&
        angle2Deg < restAngle2 &&
        angle3Deg < restAngle3)
        armDown = false;
    else
        armDown = true;

    urGovernor::SerialWrite serialWrite;
    // Pack message
    SerialUtils::CmdMsg msg = {
        .cmd_type = SerialUtils::CMDTYPE_MTRS,
        .is_relative = relativeAngleFlag,
        .mtr_angles = {(uint32_t)angle1Deg, (uint32_t)angle2Deg, (uint32_t)angle3Deg},
    };
    // Send angles to HAL (via calling the serial WRITE client)
    if (sendCmd(msg))
    {
        *p_msg = msg;
        return true;
    }
    else
    {
        ROS_ERROR("Serial write to set motors was NOT successful.");
        return false;
    }
}

// Single set point, blocks until it has been reached
bool actuateArmAngles(int angle1Deg, int angle2Deg, int angle3Deg, bool calibrate=false)
{
    bool sent = false;
    SerialUtils::CmdMsg msg;
    if (calibrate) {
        msg.cmd_type = SerialUtils::CMDTYPE_CAL;
        sent = sendCmd(msg);
    } else {
        sent = sendArmAngles(angle1Deg, angle2Deg, angle3Deg, &msg);
    }
    
    if (sent)
    {
        return waitSuccess(msg);
    }
}


// Starts the end effector actuation
bool startEndEffector()
{
    if (endEffectorRunning)
        return true;
        
    ROS_INFO("START end effector.");
    endEffectorRunning = true;
    SerialUtils::CmdMsg msg = { .cmd_type = SerialUtils::CMDTYPE_ENDEFF_ON };
    sendCmd(msg);
    if (!waitSuccess(msg)) {
        ROS_ERROR("Unable to start end effector.");
        return false;
    }
    
    return true;
}

// Stops the end effector actuation 
bool stopEndEffector()
{
    if (!endEffectorRunning)
        return true;
    
    ROS_INFO("STOP end effector.");
    endEffectorRunning = false;
    SerialUtils::CmdMsg msg = { .cmd_type = SerialUtils::CMDTYPE_ENDEFF_OFF };
    sendCmd(msg);
    if (!waitSuccess(msg)) {
        ROS_ERROR("Unable to stop end effector.");
        return false;
    }
    
    return true;
}

/* This function performs the function of 'uprooting' a weed
 *      This function polls the tracker to update the location of the weed
 */
void doConstantTrackingUproot(urGovernor::FetchWeed &fetchWeedSrv)
{
    // Continually update these angles
    int oldAngle1 = 0,oldAngle2 = 0,oldAngle3 = 0;
    // Save the current tracking ID
    int currentTrackingID = fetchWeedSrv.response.tracking_id;
    // Now we only want to query for this one
    fetchWeedSrv.request.request_id = currentTrackingID;

    static int lastIDOutOfRange = -1;

    // Time this whole operation
    ros::WallTime startActuation, startUproot;
    double timeDelta = 0;
    bool weedReached = false;

    // Set start time
    startActuation = ros::WallTime::now();

    bool keepGoing = true;
    // Do a continual update on the weeds location
    ros::Rate loopRate(overallRate);
    
    SerialUtils::CmdMsg last_msg;
    bool command_sent = false;

    // Main Loop for constant tracking
    while (ros::ok() && keepGoing)
    {
        // Get the most recent coordinates
        if (!fetchWeedClient.call(fetchWeedSrv))
        {
            keepGoing = false;  
        }
        else
        {
            //// Process the current coordinates
            float targetX = fetchWeedSrv.response.weed.point.x;
            // Add offset here to compensate for motion
            float targetY = fetchWeedSrv.response.weed.point.y + targetYGain*curYVel;
            float targetZ = fetchWeedSrv.response.weed.point.z;
            float targetSize = fetchWeedSrv.response.weed.size_cm;

            // IF cartesian coordinate are out of range
            if (targetX > cartesianLimitXMax ||
                targetX < cartesianLimitXMin ||
                targetY > cartesianLimitYMax ||
                targetY < cartesianLimitYMin ) 
            {
                if (targetY < cartesianLimitYMin)
                {
                    keepGoing = false;
                    urGovernor::RemoveWeed rmWeedSrv;
                    rmWeedSrv.request.tracking_id = currentTrackingID;
                    rmWeedClient.call(rmWeedSrv);
                }

                if (fetchWeedSrv.request.request_id != lastIDOutOfRange)
                {
                    lastIDOutOfRange = fetchWeedSrv.request.request_id;
                    ROS_INFO("COORDS OUT OF RANGE of delta arm [(x,y,size)=(%.1f,%.1f,%.1f)]",targetX,targetY,targetSize);
                }
                // We are out of range!
                keepGoing = false;
            }
            else
            {
                /* Create coordinates in the Delta Arm Reference
                *   This conversion requires a 'rotation matrix' 
                *   to be applied to comply with Delta library coordinates.
                *   x' = x*cos(theta) - y*sin(theta)
                *   y' = x*sin(theta) + y*cos(theta)
                * Based on our setup, theta = +60 degrees AND X and Y coordinates are switched
                */
                float x_coord = (float)(targetY*(0.5) - (targetX)*(0.866));
                float y_coord = (float)(targetY*(0.866) + (targetX)*(0.5));
                float z_coord = (float)targetZ + soilOffset;    // z = 0 IS AT THE GROUND (z = is always positive)

                /* Calculate angles for Delta arm */
                robot_position(x_coord, y_coord, z_coord); 
                
                // Get the resulting angles from kinematics
                int angle1Deg, angle2Deg, angle3Deg;
                getArmAngles(&angle1Deg, &angle2Deg, &angle3Deg);

                if (angle1Deg < 0)
                    angle1Deg = 0;
                if (angle2Deg < 0)
                    angle2Deg = 0;
                if (angle3Deg < 0)
                    angle3Deg = 0;

                // IF calculated angles are out of range
                if (angle1Deg > angleLimit ||
                    angle2Deg > angleLimit ||
                    angle3Deg > angleLimit ||
                    angle1Deg < 0 ||
                    angle2Deg < 0 ||
                    angle3Deg < 0 )
                {
                    ROS_INFO("ANGLES OUT OF RANGE of delta arm [(a1,a2,a3)=(%i,%i,%i)]",angle1Deg,angle2Deg,angle3Deg);
                    keepGoing = false;
                }
                // ELSE if any of the angles have changed, make call to update the arm angles
                else if(abs(angle1Deg - oldAngle1) > minUpdateAngle ||
                        abs(angle2Deg - oldAngle2) > minUpdateAngle ||
                        abs(angle3Deg - oldAngle3) > minUpdateAngle)
                {
                    // If we've already sent an arm angle and this 
                    if(command_sent && ( 
                        abs(angle1Deg - oldAngle1) > maxUpdateAngle ||
                        abs(angle2Deg - oldAngle2) > maxUpdateAngle ||
                        abs(angle3Deg - oldAngle3) > maxUpdateAngle 
                        ))
                    {
                        ROS_ERROR("Angle update is too large... skipping ...");
                    }
                    else
                    {
                        oldAngle1 = angle1Deg;
                        oldAngle2 = angle2Deg;
                        oldAngle3 = angle3Deg;

                        ROS_INFO("UPDATE weed @ (%.1f,%.1f,%.1f) [cm] -> (%i,%i,%i) [degrees]",
                            targetX, targetY, targetZ, 
                            angle1Deg, angle2Deg, angle3Deg);

                        startEndEffector();

                        // Update the arm angles
                        if (!sendArmAngles(angle1Deg, angle2Deg, angle3Deg, &last_msg))
                        {
                            // This is a Fatal issue ...
                            ROS_ERROR("Could not actuate motors to specified arm angles");
                            ros::requestShutdown();

                            keepGoing = false;
                        } else {
                            command_sent = true;
                        }
                    }
                }
            }
        }

        // IF weed has been reached by the arm
        if(weedReached)
        {
            timeDelta = (ros::WallTime::now() - startUproot).toSec();
            if (timeDelta >= endEffectorTime)
            {
                keepGoing = false;
            }
        }
        // ELSE if we haven't set our flag but the motors are done their current motion
        else if (!weedReached && command_sent && checkSuccess(last_msg))
        {
            weedReached = true;
            startUproot = ros::WallTime::now();
        }
        // ELSE
        else
        {
            timeDelta = (ros::WallTime::now() - startActuation).toSec();
            // Override if we've hit our actuation time override
            if (timeDelta >= actuationTimeOverride)
            {
                weedReached = true;
                startUproot = ros::WallTime::now();
            }
        }

        // After send the arm angle update, sleep for the loop rate
        loopRate.sleep();
    }

    // IF not weedReached
    if (!weedReached)
    {
        // Check if we should mark it as uprooted anyways
        timeDelta = (ros::WallTime::now() - startActuation).toSec();
        // Override if we've hit our actuation time override
        if (timeDelta >= actuationTimeOverride)
        {
            weedReached = true;
        }
    }

    urGovernor::MarkUprooted markUprootedSrv;
    // weedReached indicates the success of this call
    markUprootedSrv.request.success = command_sent;
    // Mark this weed as uprooted (or back to ready if not successful)
    markUprootedSrv.request.tracking_id = currentTrackingID;
    if (!markUprootedClient.call(markUprootedSrv))
    {
        ROS_INFO("Governor -- Error calling markUprooted Srv (call to tracker_node).");
    }

    return;    
}

// velocity callback from tracker
void updateVelocity(const geometry_msgs::Vector3::ConstPtr& msg){
    curYVel = msg->y;
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "urGovernor_node");
    ros::NodeHandle nh;
    ros::NodeHandle nodeHandle("~");

    int fetchWeedLogs = 0;

    if (!readGeneralParameters(nodeHandle))
    {
        ROS_ERROR("Could not read general parameters for urGovernor_node.");
        ros::requestShutdown();
    }

    serialWriteClient = nh.serviceClient<urGovernor::SerialWrite>(serialServiceWriteName);
    ros::service::waitForService(serialServiceWriteName);

    serialReadClient = nh.serviceClient<urGovernor::SerialRead>(serialServiceReadName);
    ros::service::waitForService(serialServiceReadName);

    // Subscribe to service from tracker
    fetchWeedClient = nh.serviceClient<urGovernor::FetchWeed>(fetchWeedServiceName);
    ros::service::waitForService(fetchWeedServiceName);
    urGovernor::FetchWeed fetchWeedSrv;
    urGovernor::FetchWeed fetchWeedSrvLast;

    // Subscribe to second service from tracker
    markUprootedClient = nh.serviceClient<urGovernor::MarkUprooted>(markUprootedServiceName);
    ros::service::waitForService(markUprootedServiceName);

    rmWeedClient = nh.serviceClient<urGovernor::RemoveWeed>(rmWeedServiceName);
    ros::service::waitForService(rmWeedServiceName);

    // Subscribe to velocity updates from tracker
    curYVel = 0;
    ros::Subscriber velocitySub = nodeHandle.subscribe(
                velocityPublisherName,
                1,
                updateVelocity
    );


    /* Initializing Kinematics */
    // Set tool offset (tool id == 0, x, y, z )
    robot_tool_offset(0, 0, 0, -(toolOffset));
    // Default deltarobot setup
    deltarobot_setup();

    stopEndEffector();

    // CALIBRATE arms
    if (!actuateArmAngles(restAngle1, restAngle2, restAngle3, true))
    {
        ROS_ERROR("Could not Initialize arm positions.");
        ros::requestShutdown();
    }

    // CONFIGURE motors
    if (!configMotors(motorSpeedDegS, motorAccelDegSS))
    {
        ROS_ERROR("Unable to configure motors... continuing with default speed & accel");
    }

    // Sleep for startup to ensure we get our camera stream
    ros::Duration(initSleepTime).sleep();

    /* 
     * Main loop for urGovernor
     */
    auto putArmsUp = [&] {
        if (::armDown) {
            if (!actuateArmAngles(restAngle1, restAngle2, restAngle3))
            {
                ROS_ERROR("Could not Reset arm positions.");
                ros::requestShutdown();
            }
        }
        stopEndEffector();
    };

    auto pointDist = [] (geometry_msgs::Point p1, geometry_msgs::Point p2) -> float {
        float dx = p1.x - p2.x;
        float dy = p1.y - p2.y;
        float dz = p1.z - p2.z;
        float dist = sqrt( dx*dx + dy*dy + dz*dz );
        ROS_DEBUG("Got distance: %f", dist);
        return dist;
    };

    ros::Rate loopRate(overallRate);
    while (ros::ok())
    {
        fetchWeedSrv.request.caller = 1;
        // Set to -1 to indicate we just want the top valid
        fetchWeedSrv.request.request_id = -1;

        // IF we do get a new weed
        if (fetchWeedClient.call(fetchWeedSrv))
        {
            // stay down if the weeds are close
            if (pointDist(fetchWeedSrv.response.weed.point,
                        fetchWeedSrvLast.response.weed.point) > stayDownDist)
                putArmsUp();

            doConstantTrackingUproot(fetchWeedSrv);
            fetchWeedSrvLast = fetchWeedSrv;
        }
        else
        {
            putArmsUp();
            if (fetchWeedLogs % logFetchWeedInterval == 1)
            {
                ROS_INFO("Governor -- no weeds are current.");
            }
            fetchWeedLogs++;
        }

        ros::spinOnce();
        loopRate.sleep();
    }

    return 0;
}
