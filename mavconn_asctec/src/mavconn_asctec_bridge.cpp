#include <glib.h>
#include <mavconn.h>
#include <core/MAVConnParamClient.h>
#include <ros/ros.h>

#include <asctec_hl_comm/mav_ctrl.h>
#include <asctec_hl_comm/GpsCustom.h>
#include <asctec_hl_comm/mav_status.h>
#include <asctec_hl_comm/GpsCustomCartesian.h>
#include <mav_status/Status.h>
#include <sensor_msgs/Imu.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <std_msgs/String.h>
#include <tf/transform_datatypes.h>

std::string lcmurl = "udpm://"; ///< host name for UDP server
std::string rosnamespace = "";

// Initial positions
//float mav1PositionX = 1.0f;
//float mav1PositionY = 2.0f;
//float mav1PositionZ = -4.0f;

//float mav2PositionX = 4.0f;
//float mav2PositionY = 5.0f;
//float mav2PositionZ = -4.0f;

//float mav3PositionX = 6.0f;
//float mav3PositionY = 7.0f;
//float mav3PositionZ = -4.0f;

// Offsets set
bool offsetKnown = false;

bool verbose = false;
bool fixed_offset = false;
bool sendingAllowed = false;

float transmit_setpoint = 0;
float transmit_localization = 0;
float src_gps = 0;
float src_yaw = 0;

float filterRoll = 0.0f;
float filterPitch = 0.0f;
float filterYaw = 0.0f;

typedef struct
{
	lcm_t* lcm;
	MAVConnParamClient* client;
	ros::Publisher* wp_publisher;
	ros::Publisher* pose_stamped_publisher;
} thread_context_t;

MAVConnParamClient* paramClient;

int sysid = getSystemID();
int compid = 201;

/**
 * Grabs all mavlink_local_position_setpoint_set_t messages from MAVLINK and
 * publishes the corresponding asctec_hl_comm::WaypointActionGoal messages to
 * the ROS topic "fcu/waypoint/goal".
 * Grabs all geometry_msgs/PoseStamped messages from ROS and publishes the
 * corresponding messages to MAVLINK.
 */

ros::NodeHandle* nh = 0;
lcm_t *lcm = 0;

const double WGS84_A = 6378137.0;
const double WGS84_ECCSQ = 0.00669437999013;

double homeLatitude = 0.0;
double homeLongitude = 0.0;
double homeAltitude = 0.0;

// Mapped to parameter interface
// only for initial values!
float globalOffsetX = 0.0;
float globalOffsetY = 0.0;
float globalOffsetZ = 0.0;
float globalOffsetYaw = 0.0;

double lastPTAMNEDPositionX = 0.0;
double lastPTAMNEDPositionY = 0.0; 
double lastPTAMNEDPositionZ = 0.0; 
double lastPTAMNEDPositionYaw = 0.0; 

double lastGPSNEDPositionX = 5.0;
double lastGPSNEDPositionY = 4.0;
double lastGPSNEDPositionZ = 3.0;
double lastGPSNEDPositionYaw = 0.3;

char
utmLetterDesignator(double latitude)
{
    // This routine determines the correct UTM letter designator for the given latitude
    // returns 'Z' if latitude is outside the UTM limits of 84N to 80S
    // Written by Chuck Gantz- chuck.gantz@globalstar.com
    char letterDesignator;

    if ((84.0 >= latitude) && (latitude >= 72.0)) letterDesignator = 'X';
    else if ((72.0 > latitude) && (latitude >= 64.0)) letterDesignator = 'W';
    else if ((64.0 > latitude) && (latitude >= 56.0)) letterDesignator = 'V';
    else if ((56.0 > latitude) && (latitude >= 48.0)) letterDesignator = 'U';
    else if ((48.0 > latitude) && (latitude >= 40.0)) letterDesignator = 'T';
    else if ((40.0 > latitude) && (latitude >= 32.0)) letterDesignator = 'S';
    else if ((32.0 > latitude) && (latitude >= 24.0)) letterDesignator = 'R';
    else if ((24.0 > latitude) && (latitude >= 16.0)) letterDesignator = 'Q';
    else if ((16.0 > latitude) && (latitude >= 8.0)) letterDesignator = 'P';
    else if (( 8.0 > latitude) && (latitude >= 0.0)) letterDesignator = 'N';
    else if (( 0.0 > latitude) && (latitude >= -8.0)) letterDesignator = 'M';
    else if ((-8.0 > latitude) && (latitude >= -16.0)) letterDesignator = 'L';
    else if ((-16.0 > latitude) && (latitude >= -24.0)) letterDesignator = 'K';
    else if ((-24.0 > latitude) && (latitude >= -32.0)) letterDesignator = 'J';
    else if ((-32.0 > latitude) && (latitude >= -40.0)) letterDesignator = 'H';
    else if ((-40.0 > latitude) && (latitude >= -48.0)) letterDesignator = 'G';
    else if ((-48.0 > latitude) && (latitude >= -56.0)) letterDesignator = 'F';
    else if ((-56.0 > latitude) && (latitude >= -64.0)) letterDesignator = 'E';
    else if ((-64.0 > latitude) && (latitude >= -72.0)) letterDesignator = 'D';
    else if ((-72.0 > latitude) && (latitude >= -80.0)) letterDesignator = 'C';
    else letterDesignator = 'Z'; //This is here as an error flag to show that the Latitude is outside the UTM limits

    return letterDesignator;
}

void
lltoutm(double latitude, double longitude,
		double& utmNorthing, double& utmEasting,
		std::string& utmZone)
{
    // converts lat/long to UTM coords.  Equations from USGS Bulletin 1532
    // East Longitudes are positive, West longitudes are negative.
    // North latitudes are positive, South latitudes are negative
    // Lat and Long are in decimal degrees
    // Written by Chuck Gantz- chuck.gantz@globalstar.com

    double k0 = 0.9996;

    double LongOrigin;
    double eccPrimeSquared;
    double N, T, C, A, M;

    double LatRad = latitude * M_PI / 180.0;
    double LongRad = longitude * M_PI / 180.0;
    double LongOriginRad;

    int ZoneNumber = static_cast<int>((longitude + 180.0) / 6.0) + 1;

    if (latitude >= 56.0 && latitude < 64.0 &&
            longitude >= 3.0 && longitude < 12.0) {
        ZoneNumber = 32;
    }

    // Special zones for Svalbard
    if (latitude >= 72.0 && latitude < 84.0) {
        if (     longitude >= 0.0  && longitude <  9.0) ZoneNumber = 31;
        else if (longitude >= 9.0  && longitude < 21.0) ZoneNumber = 33;
        else if (longitude >= 21.0 && longitude < 33.0) ZoneNumber = 35;
        else if (longitude >= 33.0 && longitude < 42.0) ZoneNumber = 37;
    }
    LongOrigin = static_cast<double>((ZoneNumber - 1) * 6 - 180 + 3);  //+3 puts origin in middle of zone
    LongOriginRad = LongOrigin * M_PI / 180.0;

    // compute the UTM Zone from the latitude and longitude
    std::ostringstream oss;
    oss << ZoneNumber << utmLetterDesignator(latitude);
    utmZone = oss.str();

    eccPrimeSquared = WGS84_ECCSQ / (1.0 - WGS84_ECCSQ);

    N = WGS84_A / sqrt(1.0f - WGS84_ECCSQ * sin(LatRad) * sin(LatRad));
    T = tan(LatRad) * tan(LatRad);
    C = eccPrimeSquared * cos(LatRad) * cos(LatRad);
    A = cos(LatRad) * (LongRad - LongOriginRad);

    M = WGS84_A * ((1.0 - WGS84_ECCSQ / 4.0
                    - 3.0 * WGS84_ECCSQ * WGS84_ECCSQ / 64.0
                    - 5.0 * WGS84_ECCSQ * WGS84_ECCSQ * WGS84_ECCSQ / 256.0)
                   * LatRad
                   - (3.0 * WGS84_ECCSQ / 8.0
                      + 3.0 * WGS84_ECCSQ * WGS84_ECCSQ / 32.0
                      + 45.0 * WGS84_ECCSQ * WGS84_ECCSQ * WGS84_ECCSQ / 1024.0)
                   * sin(2.0 * LatRad)
                   + (15.0 * WGS84_ECCSQ * WGS84_ECCSQ / 256.0
                      + 45.0 * WGS84_ECCSQ * WGS84_ECCSQ * WGS84_ECCSQ / 1024.0)
                   * sin(4.0 * LatRad)
                   - (35.0 * WGS84_ECCSQ * WGS84_ECCSQ * WGS84_ECCSQ / 3072.0)
                   * sin(6.0 * LatRad));

    utmEasting = k0 * N * (A + (1.0 - T + C) * A * A * A / 6.0
                           + (5.0 - 18.0 * T + T * T + 72.0 * C
                              - 58.0 * eccPrimeSquared)
                           * A * A * A * A * A / 120.0)
                 + 500000.0;

    utmNorthing = k0 * (M + N * tan(LatRad) *
                        (A * A / 2.0 +
                         (5.0 - T + 9.0 * C + 4.0 * C * C) * A * A * A * A / 24.0
                         + (61.0 - 58.0 * T + T * T + 600.0 * C
                            - 330.0 * eccPrimeSquared)
                         * A * A * A * A * A * A / 720.0));
    if (latitude < 0.0) {
        utmNorthing += 10000000.0; //10000000 meter offset for southern hemisphere
    }
}

void
utmtoll(double utmNorthing, double utmEasting, const std::string& utmZone,
        double& latitude, double& longitude)
{
    // converts UTM coords to lat/long.  Equations from USGS Bulletin 1532
    // East Longitudes are positive, West longitudes are negative.
    // North latitudes are positive, South latitudes are negative
    // Lat and Long are in decimal degrees.
    // Written by Chuck Gantz- chuck.gantz@globalstar.com

    double k0 = 0.9996;
    double eccPrimeSquared;
    double e1 = (1.0 - sqrt(1.0 - WGS84_ECCSQ)) / (1.0 + sqrt(1.0 - WGS84_ECCSQ));
    double N1, T1, C1, R1, D, M;
    double LongOrigin;
    double mu, phi1, phi1Rad;
    double x, y;
    int ZoneNumber;
    char ZoneLetter;
    bool NorthernHemisphere;

    x = utmEasting - 500000.0; //remove 500,000 meter offset for longitude
    y = utmNorthing;

    std::istringstream iss(utmZone);
    iss >> ZoneNumber >> ZoneLetter;
    if ((ZoneLetter - 'N') >= 0) {
        NorthernHemisphere = true;//point is in northern hemisphere
    } else {
        NorthernHemisphere = false;//point is in southern hemisphere
        y -= 10000000.0;//remove 10,000,000 meter offset used for southern hemisphere
    }

    LongOrigin = (ZoneNumber - 1.0) * 6.0 - 180.0 + 3.0;  //+3 puts origin in middle of zone

    eccPrimeSquared = WGS84_ECCSQ / (1.0 - WGS84_ECCSQ);

    M = y / k0;
    mu = M / (WGS84_A * (1.0 - WGS84_ECCSQ / 4.0
                         - 3.0 * WGS84_ECCSQ * WGS84_ECCSQ / 64.0
                         - 5.0 * WGS84_ECCSQ * WGS84_ECCSQ * WGS84_ECCSQ / 256.0));

    phi1Rad = mu + (3.0 * e1 / 2.0 - 27.0 * e1 * e1 * e1 / 32.0) * sin(2.0 * mu)
              + (21.0 * e1 * e1 / 16.0 - 55.0 * e1 * e1 * e1 * e1 / 32.0)
              * sin(4.0 * mu)
              + (151.0 * e1 * e1 * e1 / 96.0) * sin(6.0 * mu);
    phi1 = phi1Rad / M_PI * 180.0;

    N1 = WGS84_A / sqrt(1.0 - WGS84_ECCSQ * sin(phi1Rad) * sin(phi1Rad));
    T1 = tan(phi1Rad) * tan(phi1Rad);
    C1 = eccPrimeSquared * cos(phi1Rad) * cos(phi1Rad);
    R1 = WGS84_A * (1.0 - WGS84_ECCSQ) /
         pow(1.0 - WGS84_ECCSQ * sin(phi1Rad) * sin(phi1Rad), 1.5);
    D = x / (N1 * k0);

    latitude = phi1Rad - (N1 * tan(phi1Rad) / R1)
               * (D * D / 2.0 - (5.0 + 3.0 * T1 + 10.0 * C1 - 4.0 * C1 * C1
                                 - 9.0 * eccPrimeSquared) * D * D * D * D / 24.0
                  + (61.0 + 90.0 * T1 + 298.0 * C1 + 45.0 * T1 * T1
                     - 252.0 * eccPrimeSquared - 3.0 * C1 * C1)
                  * D * D * D * D * D * D / 720.0);
    latitude *= 180.0 / M_PI;

    longitude = (D - (1.0 + 2.0 * T1 + C1) * D * D * D / 6.0
                 + (5.0 - 2.0 * C1 + 28.0 * T1 - 3.0 * C1 * C1
                    + 8.0 * eccPrimeSquared + 24.0 * T1 * T1)
                 * D * D * D * D * D / 120.0) / cos(phi1Rad);
    longitude = LongOrigin + longitude / M_PI * 180.0;
}

void
mavStatusCallback(const mav_status::Status& statusMsg)
{
        // set timestamp (get NSec from ROS and convert to us)
        uint64_t timestamp = statusMsg.header.stamp.toNSec() / 1000;
        // send MAVLINK attitude and local position messages
        mavlink_message_t msg;
      	uint8_t aircraft_type = MAV_TYPE_HEXAROTOR;
	uint8_t ap_type = MAV_AUTOPILOT_GENERIC;

        uint32_t base_mode = MAV_MODE_FLAG_STABILIZE_ENABLED;
        uint32_t custom_mode = 0;

	/* custom mode definition:
	 *
	 * 4096: MAV_CONTROLLER_LL_GPS | 2048: MAV_CONTROLLER_LL_HEIGHT | 1024: MAV_CONTROLLER_HL_POS | 512: MAV_CONTROLLER_HL_HEIGHT | 256: MAV_CONTROLLER_MANUAL | 128:RES | 64: RES | 32: RES | 16: RES | 8: UPDATE_LOC | 4: UPDATE_PTAM | 2: UPDATE_GPS | 1: INITIALIZING
	 */

	#define MAV_CONTROLLER_MANUAL_FLAG 256
	#define MAV_CONTROLLER_HL_HEIGHT_FLAG 512

	// Analyze controller state
	switch (statusMsg.mav_controller_mode)
	{
	case mav_status::Status::MAV_CONTROLLER_MANUAL:
	base_mode |= MAV_MODE_FLAG_MANUAL_INPUT_ENABLED;
	custom_mode |= MAV_CONTROLLER_MANUAL_FLAG;
	break;
	case mav_status::Status::MAV_CONTROLLER_HL_HEIGHT:
	base_mode |= MAV_MODE_FLAG_GUIDED_ENABLED;
	custom_mode |= MAV_CONTROLLER_HL_HEIGHT_FLAG;
	break;
	case mav_status::Status::MAV_CONTROLLER_HL_POS:
	base_mode |= MAV_MODE_FLAG_AUTO_ENABLED;
        break;
	case mav_status::Status::MAV_CONTROLLER_LL_HEIGHT:
	base_mode |= MAV_MODE_FLAG_GUIDED_ENABLED;
        break;
	case mav_status::Status::MAV_CONTROLLER_LL_GPS:
	base_mode |= MAV_MODE_FLAG_GUIDED_ENABLED;
        break;
	}
	uint8_t system_status = MAV_STATE_ACTIVE;
        mavlink_msg_heartbeat_pack_chan(sysid, compid, MAVLINK_COMM_0, &msg, aircraft_type/*type*/, ap_type/*autopilot*/, base_mode/*base mode*/, custom_mode/*custom mode*/, system_status);
        sendMAVLinkMessage(lcm, &msg);
        if (verbose)
        {
                ROS_INFO("Sent Mavlink heartbeat status message");
        }
}

double fusedAttRoll = 0;
double fusedAttPitch = 0;
double fusedAttYaw = 0;

void
poseStampedCallback(const geometry_msgs::PoseStamped& poseStampedMsg)
{
	// set timestamp (get NSec from ROS and convert to us)
	uint64_t timestamp = poseStampedMsg.header.stamp.toNSec() / 1000;

	// send MAVLINK attitude and local position messages
	mavlink_message_t msg;

	//convert quaternion to euler angles
	const btQuaternion quat(poseStampedMsg.pose.orientation.x,
							poseStampedMsg.pose.orientation.y,
							poseStampedMsg.pose.orientation.z,
							poseStampedMsg.pose.orientation.w);
	const btMatrix3x3 mat(quat);
	double roll, pitch, yaw;
	mat.getEulerYPR(yaw, pitch, roll);

	// PTAMYAW ENU to NED yaw
	fusedAttYaw = fmod(-yaw+M_PI/2+M_PI, 2.*M_PI)-M_PI;

	//mavlink_msg_attitude_pack_chan(sysid, compid, MAVLINK_COMM_0, &msg, timestamp, fusedAttRoll, fusedAttYaw, fmod(-fusedAttYaw+M_PI/2, 2.*M_PI), 0.0f, 0.0f, 0.0f);
	//sendMAVLinkMessage(lcm, &msg);

	float x = poseStampedMsg.pose.position.y;
	float y = poseStampedMsg.pose.position.x;
	float z = -poseStampedMsg.pose.position.z;

	// Set last position values
	lastPTAMNEDPositionX = x;
	lastPTAMNEDPositionY = y; 
	lastPTAMNEDPositionZ = z; 
	lastPTAMNEDPositionYaw = fusedAttYaw; 

	if (offsetKnown)
	{
		// Send changed message
		mavlink_msg_local_position_ned_pack_chan(sysid, 201, MAVLINK_COMM_0, &msg, timestamp,
		x+paramClient->getParamValue("POS-OFFSET_X"),
		y+paramClient->getParamValue("POS-OFFSET_Y"),
		z+paramClient->getParamValue("POS-OFFSET_Z"), 0.0f, 0.0f, 0.0f);
        	sendMAVLinkMessage(lcm, &msg);

		// Send unchanged message
		mavlink_msg_local_position_ned_pack_chan(sysid, 203, MAVLINK_COMM_2, &msg, timestamp, x, y, z, 0.0f, 0.0f, 0.0f);
                sendMAVLinkMessage(lcm, &msg);
	}
	else
	{
		mavlink_msg_local_position_ned_pack_chan(sysid, 201, MAVLINK_COMM_0, &msg, timestamp, x, y, z, 0.0f, 0.0f, 0.0f);
		sendMAVLinkMessage(lcm, &msg);
	}

	if (verbose)
	{
		ROS_INFO("Sent Mavlink local position and attitude messages.");
	}

	filterRoll = roll;
	filterPitch = pitch;
	filterYaw = yaw;
}

void
poseGpsEnuCallback(const asctec_hl_comm::GpsCustomCartesian& poseStampedMsg)
{
        // set timestamp (get NSec from ROS and convert to us)
        uint64_t timestamp = poseStampedMsg.header.stamp.toNSec() / 1000;

        // send MAVLINK attitude and local position messages
        mavlink_message_t msg;

	// Convert ENU to NED
        float x = poseStampedMsg.position.y;
        float y = poseStampedMsg.position.x;
        float z = -poseStampedMsg.position.z;
	float vx = poseStampedMsg.velocity_y;
	float vy = poseStampedMsg.velocity_x;

	lastGPSNEDPositionX = x;  
        lastGPSNEDPositionY = y; 
        lastGPSNEDPositionZ = z; 
        lastGPSNEDPositionYaw = fusedAttYaw; 

	if (offsetKnown)
        {
                // Send changed message
                mavlink_msg_local_position_ned_pack_chan(sysid, 202, MAVLINK_COMM_1, &msg, timestamp,
                x+paramClient->getParamValue("POS-OFFSET_X"),
                y+paramClient->getParamValue("POS-OFFSET_Y"),
                z+paramClient->getParamValue("POS-OFFSET_Z"), vx, vy, 0.0f);
                sendMAVLinkMessage(lcm, &msg);

                // Send unchanged message
                mavlink_msg_local_position_ned_pack_chan(sysid, 204, MAVLINK_COMM_3, &msg, timestamp, x, y, z, 0.0f, 0.0f, 0.0f);
                sendMAVLinkMessage(lcm, &msg);
        }
        else
        {
                mavlink_msg_local_position_ned_pack_chan(sysid, 202, MAVLINK_COMM_1, &msg, timestamp, x, y, z, vx, vy, 0.0f);
                sendMAVLinkMessage(lcm, &msg);
        }

        if (verbose)
        {
                ROS_INFO("Sent Mavlink NED cartesian coordinates message");
        }

	mavlink_msg_attitude_pack_chan(sysid, 202, MAVLINK_COMM_1, &msg, timestamp, filterRoll, filterPitch, fmod(-filterYaw+M_PI/2, 2.*M_PI), 0.0f, 0.0f, 0.0f);
	sendMAVLinkMessage(lcm, &msg);
}

void
fcuStatusCallback(const asctec_hl_comm::mav_status& status)
{           
        // send MAVLINK attitude and local position messages
        mavlink_message_t msg;
        
        int vbat = status.battery_voltage*1000;
	int curr = -1;
	int battery_remaining = -1;
	int cpu_load = status.cpu_load;
	int tx_err = status.tx_packets - status.tx_packets_good;
	int rx_err = status.rx_packets - status.rx_packets_good;                
	int drop_comm = tx_err+rx_err;
	int err_comm = tx_err+rx_err;

	uint32_t control_sensors_present = 0;
	uint32_t control_sensors_enabled = 0;
	uint32_t control_sensors_health = 0;

        mavlink_msg_sys_status_pack_chan(sysid, compid, MAVLINK_COMM_0, &msg, control_sensors_present,
							 control_sensors_enabled,
							 control_sensors_health,
	                            cpu_load, vbat, curr, battery_remaining, drop_comm, err_comm,
			            tx_err, rx_err, 0, 0);
        sendMAVLinkMessage(lcm, &msg);
        
        if (verbose)
        {
                ROS_INFO("Sent Mavlink status message.");
        }
}

void
schoofCallback(const std_msgs::String& string)
{   
        // send MAVLINK attitude and local position messages
        mavlink_message_t msg;
        uint32_t timestamp = 0;

	offsetKnown = true;

	if (fixed_offset)
	{
		if (sysid == 1)
		{
			globalOffsetX = paramClient->getParamValue("MAV1-INITOFF_X")-lastPTAMNEDPositionX;
                        globalOffsetY = paramClient->getParamValue("MAV1-INITOFF_Y")-lastPTAMNEDPositionY;
                        globalOffsetZ = 0.; 
		}
		if (sysid == 2)
                {
                        globalOffsetX = paramClient->getParamValue("MAV2-INITOFF_X")-lastPTAMNEDPositionX;
                        globalOffsetY = paramClient->getParamValue("MAV2-INITOFF_Y")-lastPTAMNEDPositionY;
                        globalOffsetZ = 0.; 
                }
		if (sysid == 3)
                {
			globalOffsetX = paramClient->getParamValue("MAV3-INITOFF_X")-lastPTAMNEDPositionX;
                        globalOffsetY = paramClient->getParamValue("MAV3-INITOFF_Y")-lastPTAMNEDPositionY;
                        globalOffsetZ = 0.; 
                }
	}
	else
	{
		globalOffsetX = lastGPSNEDPositionX - lastPTAMNEDPositionX;
		globalOffsetY = lastGPSNEDPositionY - lastPTAMNEDPositionY;
		globalOffsetZ = lastGPSNEDPositionZ - lastPTAMNEDPositionZ;
		globalOffsetYaw = lastGPSNEDPositionYaw - lastPTAMNEDPositionYaw;
	}

	paramClient->setParamValue("POS-OFFSET_X", globalOffsetX);
	paramClient->setParamValue("POS-OFFSET_Y", globalOffsetY);
	paramClient->setParamValue("POS-OFFSET_Z", globalOffsetZ);
	paramClient->setParamValue("POS-OFFSET_YAW", globalOffsetYaw);

        mavlink_msg_local_position_ned_system_global_offset_pack_chan(sysid, compid, MAVLINK_COMM_0, &msg, timestamp, globalOffsetX, globalOffsetY, globalOffsetZ, 0/* roll */, 0/* pitch */, globalOffsetYaw);
        sendMAVLinkMessage(lcm, &msg);
        
        if (verbose)
        {
                ROS_INFO("Sent GLOBAL offset");
        }
}


void
fcuGpsCallback(const asctec_hl_comm::GpsCustom& gpsMsg)
{   
        // set timestamp (get NSec from ROS and convert to us)
        uint64_t timestamp = gpsMsg.header.stamp.toNSec() / 1000;
        
        // send MAVLINK attitude and local position messages
        mavlink_message_t msg;
        
	sensor_msgs::NavSatStatus status = gpsMsg.status;
	int fix = 3;
	double lat = gpsMsg.latitude*1E7;
	double lon = gpsMsg.longitude*1E7;
	double alt = gpsMsg.altitude*1E3;
	if (isnan(alt) || isinf(alt)) alt = 0;
	double vdop = 0;//gpsMsg.position_covariance;
	double vx = gpsMsg.velocity_x;
	double vy = gpsMsg.velocity_y;
	double vsum = sqrt(vx*vx+vy*vy);
	double pres_alt = gpsMsg.pressure_height;
                
        mavlink_msg_gps_raw_int_pack_chan(sysid, compid, MAVLINK_COMM_0, &msg, timestamp, fix, lat, lon, alt, vdop, vdop, vsum, 0, fix);
        sendMAVLinkMessage(lcm, &msg);
        
        if (verbose)
        {
                ROS_INFO("Sent Mavlink GPS message.");
        }
}

void
fcuImuCallback(const sensor_msgs::Imu& imuMsg)
{
        // set timestamp (get NSec from ROS and convert to us)
        uint64_t timestamp = imuMsg.header.stamp.toNSec() / 1000;

        // send MAVLINK attitude and local position messages
        mavlink_message_t msg;

        //convert quaternion to euler angles
        const btQuaternion quat(imuMsg.orientation.x,
                                                        imuMsg.orientation.y,
                                                        imuMsg.orientation.z,
                                                        imuMsg.orientation.w);
        const btMatrix3x3 mat(quat);
        double roll, pitch, yaw;
        mat.getEulerYPR(yaw, pitch, roll);

	fusedAttRoll = roll;
	fusedAttPitch = pitch;

        mavlink_msg_attitude_pack_chan(sysid, compid, MAVLINK_COMM_0, &msg, timestamp, fusedAttRoll, fusedAttPitch, fusedAttYaw, 0.0f, 0.0f, 0.0f);
        sendMAVLinkMessage(lcm, &msg);
	mavlink_msg_attitude_pack_chan(sysid, compid+1, MAVLINK_COMM_1, &msg, timestamp, fusedAttRoll, fusedAttPitch, fusedAttYaw, 0.0f, 0.0f, 0.0f);
        sendMAVLinkMessage(lcm, &msg);

        if (verbose)
        {
                ROS_INFO("Sent Mavlink attitude messages.");
        }
}


void
paramCheckCallback(const ros::TimerEvent&)
{
	bool homeShift = false;

	double latitude;
	if (nh->getParamCached("/gps_ref_latitude", latitude) &&
		latitude != homeLatitude)
	{
		homeLatitude = latitude;
		homeShift = true;
	}

	double longitude;
	if (nh->getParamCached("/gps_ref_longitude", longitude) &&
		longitude != homeLongitude)
	{
		homeLongitude = longitude;
		homeShift = true;
	}

	double altitude;
	if (nh->getParamCached("/gps_ref_altitude", altitude) &&
		altitude != homeAltitude)
	{
		homeAltitude = altitude;
		homeShift = true;
	}

	if (homeShift)
	{
		mavlink_message_t msg;
		mavlink_msg_gps_global_origin_pack_chan(sysid, compid, MAVLINK_COMM_0, &msg,
				homeLatitude * 10000000.0, homeLongitude * 10000000, homeAltitude * 1000);
		//mavlink_message_t_publish(lcm, "MAVLINK", &msg);
		sendMAVLinkMessage(lcm, &msg);

		if (verbose)
		{
			ROS_INFO("Sent Mavlink GPS local origin set message.");
		}
	}
}

void
mavlinkHandler(const lcm_recv_buf_t* rbuf, const char* channel,
			   const mavconn_mavlink_msg_container_t* container, void* user)
{

	thread_context_t* context = static_cast<thread_context_t*>(user);

	lcm_t* lcm = context->lcm;
	const mavlink_message_t* msg = getMAVLinkMsgPtr(container);

	// Handle param messages
	context->client->handleMAVLinkPacket(msg);
	ros::Publisher* waypointPub = context->wp_publisher;
	ros::Publisher* poseStampedPub = context->pose_stamped_publisher;

	switch (msg->msgid)
	{
		// get setpoint from MAVLINK
		case MAVLINK_MSG_ID_SET_LOCAL_POSITION_SETPOINT:
		{
			mavlink_set_local_position_setpoint_t setpoint;
			mavlink_msg_set_local_position_setpoint_decode(msg, &setpoint);

			// publish goal to ROS
			double yawtemp=-(setpoint.yaw - 90)/180.0f*M_PI;
//			double yawtemp=-fusedAttYaw + M_PI/2.f;
			asctec_hl_comm::mav_ctrl goal;
			goal.header.stamp = ros::Time::now();
			
			// OFFSET ADDED TO SETPOINT
			if (offsetKnown)
			{
				goal.x = (setpoint.y - paramClient->getParamValue("POS-OFFSET_Y"));
				goal.y = (setpoint.x - paramClient->getParamValue("POS-OFFSET_X"));
				goal.z = -(setpoint.z - paramClient->getParamValue("POS-OFFSET_Z"));
			}
			else
			{
				goal.x = setpoint.y;
                        	goal.y = setpoint.x;
                        	goal.z = -setpoint.z;
			}
			goal.yaw = (yawtemp>M_PI)?yawtemp-2*M_PI:yawtemp; 
     			goal.v_max_xy = 2.0f;
			goal.v_max_z = 2.0f;
			goal.type = asctec_hl_comm::mav_ctrl::position;
						
			
			if (paramClient->getParamValue("SP-SEND") == 1 && sendingAllowed)
			{
				waypointPub->publish(goal);
				// Echo back setpoint
				mavlink_local_position_setpoint_t new_setpoint;
				new_setpoint.x = setpoint.x;
				new_setpoint.y = setpoint.y;
                                new_setpoint.z = setpoint.z;
                                new_setpoint.yaw = setpoint.yaw/180.0f*M_PI;
			        new_setpoint.coordinate_frame = setpoint.coordinate_frame;
				mavlink_message_t tx_msg;
				// 201 is default component id (MAVLINK_COMM_0, no need to use pack_chan here
				mavlink_msg_local_position_setpoint_encode(sysid, 201,&tx_msg, &new_setpoint);

				sendMAVLinkMessage(lcm, &tx_msg);

				if (verbose)
				{
					ROS_INFO("Sent ROS WaypointActionGoal message [%.2f %.2f %.2f %.2f].",
						 setpoint.x, setpoint.y, setpoint.z, setpoint.yaw);
				}
			}

			break;
		}
		case MAVLINK_MSG_ID_GLOBAL_VISION_POSITION_ESTIMATE:
		{
			if (msg->compid == 120 && paramClient->getParamValue("GLOB-SEND") == 1 && sendingAllowed)
                        {
				mavlink_global_vision_position_estimate_t pos;
				mavlink_msg_global_vision_position_estimate_decode(msg, &pos);
				geometry_msgs::PoseWithCovarianceStamped poseStampedMsg;
			
				double tx = pos.y;
				double ty = pos.x;
				double tz = -pos.z;

				double cRh, sRh, cPh, sPh, cYh, sYh;

				double r = fusedAttRoll;
				double p = fusedAttPitch;

				// FIXME Take yaw from vision
				// GLOBALYAW NED to PTAM ENU yaw
				double y = fmod(-pos.yaw+(M_PI/2.0)+M_PI, 2.0*M_PI)-M_PI;

				cRh = cos(r/2.0);
				sRh = sin(r/2.0);
				cPh = cos(p/2.0);
				sPh = sin(p/2.0);
				cYh = cos(y/2.0);
				sYh = sin(y/2.0);

				double w, x, z;

				w = cRh*cPh*cYh + sRh*sPh*sYh;
				x = sRh*cPh*cYh - cRh*sPh*sYh;
				y = cRh*sPh*cYh + sRh*cPh*sYh;
				z = cRh*cPh*sYh - sRh*sPh*cYh;


				poseStampedMsg.pose.pose.orientation.x = x;
                                poseStampedMsg.pose.pose.orientation.y = y;
                                poseStampedMsg.pose.pose.orientation.z = z;
                                poseStampedMsg.pose.pose.orientation.w = w;


				poseStampedMsg.pose.pose.position.x = tx;
				poseStampedMsg.pose.pose.position.y = ty;
				poseStampedMsg.pose.pose.position.z = tz;
			
				// Set covariance of vision to 0.5m std dev
				poseStampedMsg.pose.covariance[0] = 0.5f*0.5f;
				poseStampedMsg.pose.covariance[7] = 0.5f*0.5f;
				poseStampedMsg.pose.covariance[14] = 0.5f*0.5f;
				// Set covariance of vision angle to almost pi
				poseStampedMsg.pose.covariance[21] = 2.8f*2.8f;
				poseStampedMsg.pose.covariance[28] = 2.8f*2.8f;
				poseStampedMsg.pose.covariance[35] = 2.8f*2.8f;

				poseStampedPub->publish(poseStampedMsg);
			}
		}
			break;
		case MAVLINK_MSG_ID_COMMAND_LONG:
{
		mavlink_command_long_t cmd;
		mavlink_msg_command_long_decode(msg, &cmd);
                if (cmd.command == 144)
		{
			if (cmd.param1 == 1.0f)
			{
				sendingAllowed = true;
			}
			else
			{
				sendingAllowed = false;
			}
		}
}
break;
	
		default: {}
	};
}

// Handling Program options
static GOptionEntry entries[] =
{
		{ "sysid", 'a', 0, G_OPTION_ARG_INT, &sysid, "ID of this system, 1-255", "42" },
		{ "compid", 'c', 0, G_OPTION_ARG_INT, &compid, "ID of this component, 1-255", "199" },
		{ "lcmurl", 'l', 0, G_OPTION_ARG_STRING, &lcmurl, "LCM URL to connect to", "udpm://" },
		{ "rosnamespace", 'r', 0, G_OPTION_ARG_STRING, &rosnamespace, "ROS Namespace", "robot/" },
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Verbose output", NULL },
		{ "fixed-offset", 'f', 0, G_OPTION_ARG_NONE, &fixed_offset, "Fixed parameter-based offset", NULL },
		{ NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, 0 } };

void* lcm_wait(void* lcm_ptr)
{
	lcm_t* lcm = (lcm_t*) lcm_ptr;
	// Blocking wait for new data
	while (1)
	{
		lcm_handle(lcm);
		if (ros::isShuttingDown()) break;
	}
	return NULL;
}

int main(int argc, char **argv)
{
	ros::init(argc, argv, "mavconn_asctec_bridge");

	GError* error = NULL;
	GOptionContext* context = g_option_context_new("- translate messages between Asctec (ROS) and MAVCONN (LCM)");
	g_option_context_add_main_entries(context, entries, NULL);
	//g_option_context_add_group (context, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error))
	{
		g_print("Option parsing failed: %s\n", error->message);
		exit(1);
	}

	nh = new ros::NodeHandle;
	ros::Subscriber poseStampedSub = nh->subscribe((rosnamespace + std::string("fcu/current_pose")).c_str(), 10, poseStampedCallback);
	ros::Subscriber fcuImuSub = nh->subscribe((rosnamespace + std::string("fcu/imu")).c_str(), 10, fcuImuCallback);	
	ros::Subscriber fcuGpsCustomSub = nh->subscribe((rosnamespace + std::string("fcu/gps_custom")).c_str(), 10, fcuGpsCallback);
	ros::Subscriber poseGpsEnuSub = nh->subscribe((rosnamespace + std::string("fcu/gps_position_custom")).c_str(), 10, poseGpsEnuCallback);
	ros::Subscriber fcuStatusSub = nh->subscribe((rosnamespace + std::string("fcu/status")).c_str(), 10, fcuStatusCallback);
	ros::Subscriber schoofSub = nh->subscribe((rosnamespace + std::string("schoof")).c_str(), 10, schoofCallback);
	ros::Publisher waypointPub = nh->advertise<asctec_hl_comm::mav_ctrl>((rosnamespace + std::string("fcu/control")).c_str(), 10);
	ros::Publisher poseStampedPub = nh->advertise<geometry_msgs::PoseStamped>((rosnamespace + std::string("sensor_fusion/cvg_pose_no_cov")).c_str(), 10);
	ros::Publisher poseCovStampedPub = nh->advertise<geometry_msgs::PoseWithCovarianceStamped>((rosnamespace + std::string("sensor_fusion/cvg_pose")).c_str(), 10);

	ros::Subscriber statusSub = nh->subscribe((rosnamespace + std::string("mav_status")).c_str(), 10, mavStatusCallback);

	ROS_INFO("mavconn_asctec: Subscribed to pose stamped at: %s\n", (rosnamespace + std::string("fcu/current_pose")).c_str());

	// check for changed parameters on parameter server
	ros::Timer paramCheckTimer = nh->createTimer(ros::Duration(2.0), paramCheckCallback);

	/**
	 * Connect to LCM Channel and register for MAVLink messages ->
	 */
	lcm = lcm_create(lcmurl.c_str());
	if (!lcm)
	{
		exit(EXIT_FAILURE);
	}

	// Initialize parameter client before subscribing (and receiving) MAVLINK messages
	paramClient = new MAVConnParamClient(getSystemID(), compid, lcm, "mavconn-asctec-bridge.cfg", verbose);
	paramClient->setParamValue("SP-SEND", transmit_setpoint);
	paramClient->setParamValue("GLOB-SEND", transmit_localization);
	paramClient->setParamValue("GPS-SRC", src_gps);
	paramClient->setParamValue("YAW-SRC", src_yaw);
	paramClient->setParamValue("POS-OFFSET_X", globalOffsetX);
	paramClient->setParamValue("POS-OFFSET_Y", globalOffsetY);
	paramClient->setParamValue("POS-OFFSET_Z", globalOffsetZ);

	paramClient->setParamValue("MAV1-INITOFF_X", 0.0f);
	paramClient->setParamValue("MAV1-INITOFF_Y", 0.0f);
        paramClient->setParamValue("MAV1-INITOFF_Z", 0.0f);

        paramClient->setParamValue("MAV2-INITOFF_X", 0.0f);
        paramClient->setParamValue("MAV2-INITOFF_Y", 0.0f);
        paramClient->setParamValue("MAV2-INITOFF_Z", 0.0f);

        paramClient->setParamValue("MAV3-INITOFF_X", 0.0f);
        paramClient->setParamValue("MAV3-INITOFF_Y", 0.0f);
        paramClient->setParamValue("MAV3-INITOFF_Z", 0.0f);

	//paramClient->setParamValue("POS-OFFSET_YAW", globalOffsetYaw);


	paramClient->readParamsFromFile("mavconn-asctec-bridge.cfg");

	thread_context_t thread_context;
	thread_context.lcm = lcm;
	thread_context.client = paramClient;
	thread_context.wp_publisher = &waypointPub;
	thread_context.pose_stamped_publisher = &poseCovStampedPub;

	mavconn_mavlink_msg_container_t_subscription_t* mavlinkSub =
		mavconn_mavlink_msg_container_t_subscribe(lcm, "MAVLINK", &mavlinkHandler, &thread_context);

	// Initialize LCM receiver thread
	GThread* lcm_thread;
	GError* err;
	
	if( !g_thread_supported() )
	{
		g_thread_init(NULL);
		// Only initialize g thread if not already done
	}
	
	if( (lcm_thread = g_thread_create((GThreadFunc)lcm_wait, (void *)lcm, TRUE, &err)) == NULL)
	{
		printf("Thread creation failed: %s!!\n", err->message );
		g_error_free ( err ) ;
	}

	ros::spin();

	while (ros::ok())
	{
		usleep(1000*1000);
	}

	delete nh;

	mavconn_mavlink_msg_container_t_unsubscribe(lcm, mavlinkSub);
	lcm_destroy(lcm);
	g_thread_join(lcm_thread);

	return 0;
}
