#include <iostream>
#include <string>

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/photo/photo.hpp>
#include <opencv2/calib3d/calib3d.hpp>

#include "rclcpp/rclcpp.hpp"

#include "std_msgs/msg/string.hpp"
#include "std_msgs/msg/bool.hpp"
#include "std_msgs/msg/int32.hpp"

#include "geometry_msgs/msg/point.hpp"
#include "geometry_msgs/msg/pose_stamped.hpp"
#include "sensor_msgs/image_encodings.hpp"

#include "image_transport/image_transport.h"
#include "cv_bridge/cv_bridge.h"

#include "aruco/msg/marker.hpp"

#include "../ArucoMarker.cpp"
#include "../ArucoMarkerInfo.cpp"
#include "../ArucoDetector.cpp"

using namespace cv;
using namespace std;

/**
 * Camera calibration matrix pre initialized with calibration values for the test camera.
 */
double data_calibration[9] = {570.3422241210938, 0, 319.5, 0, 570.3422241210938, 239.5, 0, 0, 1};
Mat calibration;

/**
 * Lenses distortion matrix initialized with values for the test camera.
 */
double data_distortion[5] = {0, 0, 0, 0, 0};
Mat distortion;

/**
 * List of known of markers, to get the absolute position and rotation of the camera, some of these are required.
 */
vector<ArucoMarkerInfo> known = vector<ArucoMarkerInfo>();

/**
 * ROS node visibility publisher.
 * Publishes true when a known marker is visible, publishes false otherwise.
 */
rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr pub_visible;

/**
 * ROS node position publisher.
 */
rclcpp::Publisher<geometry_msgs::msg::Point>::SharedPtr pub_position;

/**
 * ROS node rotation publisher.
 */
rclcpp::Publisher<geometry_msgs::msg::Point>::SharedPtr pub_rotation;

/**
 * ROS node pose publisher.
 */
rclcpp::Publisher<geometry_msgs::msg::PoseStamped>::SharedPtr pub_pose;

/**
 * Pose publisher sequence counter.
 */
int pub_pose_seq = 0;

/**
 * Flag to check if calibration parameters were received.
 * If set to false the camera will be calibrated when a camera info message is received.
 */
bool calibrated;

/**
 * Flag to determine if OpenCV or ROS coordinates are used.
 */
bool use_opencv_coords;

/**
 * When debug parameter is se to true the node creates a new cv window to show debug information.
 * By default is set to false.
 * If set true the node will open a debug window.
 */
bool debug;

/**
 * Cosine limit used during the quad detection phase.
 * Value between 0 and 1.
 * By default 0.8 is used.
 * The bigger the value more distortion tolerant the square detection will be.
 */
float cosine_limit;

/**
 * Maximum error to be used by geometry poly aproximation method in the quad detection phase.
 * By default 0.035 is used.
 */
float max_error_quad;

/**
 * Adaptive theshold pre processing block size.
 */
int theshold_block_size;

/**
 * Minimum threshold block size.
 * By default 5 is used.
 */
int theshold_block_size_min;

/**
 * Maximum threshold block size.
 * By default 9 is used.
 */
int theshold_block_size_max;

/**
 * Minimum area considered for aruco markers.
 * Should be a value high enough to filter blobs out but detect the smallest marker necessary.
 * By default 100 is used.
 */
int min_area;

/**
 * Draw yellow text with black outline into a frame.
 * @param frame Frame mat.
 * @param text Text to be drawn into the frame.
 * @param point Position of the text in frame coordinates.
 */
void drawText(Mat frame, string text, Point point)
{
	putText(frame, text, point, FONT_HERSHEY_SIMPLEX, 0.5, Scalar(0, 0, 0), 2, CV_AA);
	putText(frame, text, point, FONT_HERSHEY_SIMPLEX, 0.5, Scalar(0, 255, 255), 1, CV_AA);
}

/**
 * Callback executed every time a new camera frame is received.
 * This callback is used to process received images and publish messages with camera position data if any.
 */
void onFrame(const sensor_msgs::msg::Image::SharedPtr msg)
{
	try
	{
		
		Mat frame = cv_bridge::toCvShare(msg, "bgr8")->image;

		//Process image and get markers
		vector<ArucoMarker> markers = ArucoDetector::getMarkers(frame, cosine_limit, theshold_block_size, min_area, max_error_quad);


		//Visible
		vector<ArucoMarker> found;

		//Vector of points
		vector<Point2f> projected;
		vector<Point3f> world;

		if(markers.size() == 0)
		{
			theshold_block_size += 2;

			if(theshold_block_size > theshold_block_size_max)
			{
				theshold_block_size = theshold_block_size_min;
			}
		}

		//Check known markers and build known of points
		for(unsigned int i = 0; i < markers.size(); i++)
		{
            //std::cerr << "got marker: " << markers[i].id << " markers " << std::endl;
            for(unsigned int j = 0; j < known.size(); j++)
			{
				if(markers[i].id == known[j].id)
				{
					markers[i].attachInfo(known[j]);
					
					for(unsigned int k = 0; k < 4; k++)
					{
						projected.push_back(markers[i].projected[k]);
						world.push_back(known[j].world[k]);
					}

					found.push_back(markers[i]);
				}
			}
		}

		//Draw markers
		if(debug)
		{
			ArucoDetector::drawMarkers(frame, markers, calibration, distortion);
		}

		//Check if any marker was found
		if(world.size() > 0)
		{
			//Calculate position and rotation
			Mat rotation, position;

			#if CV_MAJOR_VERSION == 2
				solvePnP(world, projected, calibration, distortion, rotation, position, false, ITERATIVE);
			#else
				solvePnP(world, projected, calibration, distortion, rotation, position, false, SOLVEPNP_ITERATIVE);
			#endif

			//Invert position and rotation to get camera coords
			Mat rodrigues;
			Rodrigues(rotation, rodrigues);
			
			Mat camera_rotation;
			Rodrigues(rodrigues.t(), camera_rotation);
			
			Mat camera_position = -rodrigues.t() * position;

			//Publish position and rotation
            geometry_msgs::msg::Point message_position, message_rotation;

			//Opencv coordinates
			if(use_opencv_coords)
			{
				message_position.x = camera_position.at<double>(0, 0);
				message_position.y = camera_position.at<double>(1, 0);
				message_position.z = camera_position.at<double>(2, 0);
				
				message_rotation.x = camera_rotation.at<double>(0, 0);
				message_rotation.y = camera_rotation.at<double>(1, 0);
				message_rotation.z = camera_rotation.at<double>(2, 0);
			}
			//Robot coordinates
			else
			{
				message_position.x = camera_position.at<double>(2, 0);
				message_position.y = -camera_position.at<double>(0, 0);
				message_position.z = -camera_position.at<double>(1, 0);
				
				message_rotation.x = camera_rotation.at<double>(2, 0);
				message_rotation.y = -camera_rotation.at<double>(0, 0);
				message_rotation.z = -camera_rotation.at<double>(1, 0);
			}

            pub_position->publish(message_position);
            pub_rotation->publish(message_rotation);

			//Publish pose
            geometry_msgs::msg::PoseStamped message_pose;

			//Header
			message_pose.header.frame_id = "aruco";
            //message_pose.header.seq = pub_pose_seq++;
            using builtin_interfaces::msg::Time;
            rclcpp::Clock ros_clock(RCL_ROS_TIME);
            Time ros_now = ros_clock.now();
            message_pose.header.stamp = ros_now;

			//Position
			message_pose.pose.position.x = message_position.x;
			message_pose.pose.position.y = message_position.y;
			message_pose.pose.position.z = message_position.z;

			//Convert to quaternion
			double x = message_rotation.x;
			double y = message_rotation.y;
			double z = message_rotation.z;

			//Module of angular velocity
			double angle = sqrt(x*x + y*y + z*z);

			if(angle > 0.0)
			{
				message_pose.pose.orientation.x = x * sin(angle/2.0)/angle;
				message_pose.pose.orientation.y = y * sin(angle/2.0)/angle;
				message_pose.pose.orientation.z = z * sin(angle/2.0)/angle;
				message_pose.pose.orientation.w = cos(angle/2.0);
			}
			//To avoid illegal expressions
			else
			{
				message_pose.pose.orientation.x = 0.0;
				message_pose.pose.orientation.y = 0.0;
				message_pose.pose.orientation.z = 0.0;
				message_pose.pose.orientation.w = 1.0;
			}
			
            pub_pose->publish(message_pose);

			//Debug
			if(debug)
			{
                ArucoDetector::drawOrigin(frame, found, calibration, distortion, 0.3);
				
				drawText(frame, "Position: " + to_string(message_position.x) + ", " + to_string(message_position.y) + ", " + to_string(message_position.z), Point2f(10, 180));
				drawText(frame, "Rotation: " + to_string(message_rotation.x) + ", " + to_string(message_rotation.y) + ", " + to_string(message_rotation.z), Point2f(10, 200));
			}
		}
		else if(debug)
		{
			drawText(frame, "Position: unknown", Point2f(10, 180));
			drawText(frame, "Rotation: unknown", Point2f(10, 200));
		}

		//Publish visible
        std_msgs::msg::Bool message_visible;
		message_visible.data = world.size() != 0;
        pub_visible->publish(message_visible);

		//Debug info
		if(debug)
		{
			drawText(frame, "Aruco ROS Debug", Point2f(10, 20));
			drawText(frame, "OpenCV V" + to_string(CV_MAJOR_VERSION) + "." + to_string(CV_MINOR_VERSION), Point2f(10, 40));
			drawText(frame, "Cosine Limit (A-Q): " + to_string(cosine_limit), Point2f(10, 60));
			drawText(frame, "Threshold Block (W-S): " + to_string(theshold_block_size), Point2f(10, 80));
			drawText(frame, "Min Area (E-D): " + to_string(min_area), Point2f(10, 100));
			drawText(frame, "MaxError PolyDP (R-F): " + to_string(max_error_quad), Point2f(10, 120));
			drawText(frame, "Visible: " + to_string(message_visible.data), Point2f(10, 140));
			drawText(frame, "Calibrated: " + to_string(calibrated), Point2f(10, 160));

			imshow("Aruco", frame);

			char key = (char) waitKey(1);

			if(key == 'q')
			{
				cosine_limit += 0.05;
			}
			else if(key == 'a')
			{
				cosine_limit -= 0.05;
			}

			if(key == 'w')
			{
				theshold_block_size += 2;
			}
			else if(key == 's' && theshold_block_size > 3)
			{
				theshold_block_size -= 2;
			}

			if(key == 'r')
			{
				max_error_quad += 0.005;
			}
			else if(key == 'f')
			{
				max_error_quad -= 0.005;
			}

			if(key == 'e')
			{
				min_area += 50;
			}
			else if(key == 'd')
			{
				min_area -= 50;
			}
		}
	}
	catch(cv_bridge::Exception& e)
	{
        std::cerr << "Error getting image data" << std::endl;
	}
}

/**
 * On camera info callback.
 * Used to receive camera calibration parameters.
 */
void onCameraInfo(const sensor_msgs::msg::CameraInfo::SharedPtr msg)
{
	if(!calibrated)
	{
		calibrated = true;
		
		for(unsigned int i = 0; i < 9; i++)
		{
            calibration.at<double>(i / 3, i % 3) = msg->k[i];
		}
		
		for(unsigned int i = 0; i < 5; i++)
		{
            distortion.at<double>(0, i) = msg->d[i];
		}

		if(debug)
		{
			cout << "Camera calibration param received" << endl;
			cout << "Camera: " << calibration << endl;
			cout << "Distortion: " << distortion << endl;
		}
	}
}

/**
 * Callback to register markers on the marker list.
 * This callback received a custom marker message.
 */
void onMarkerRegister(const aruco::msg::Marker::SharedPtr msg)
{
	for(unsigned int i = 0; i < known.size(); i++)
	{
        if(known[i].id == msg->id)
		{
			known.erase(known.begin() + i);
            cout << "Marker " << to_string(msg->id) << " already exists, was replaced." << endl;
			break;
		}
	}

    known.push_back(ArucoMarkerInfo(msg->id, msg->size, Point3d(msg->posx, msg->posy, msg->posz), Point3d(msg->rotx, msg->roty, msg->rotz)));
    cout << "Marker " << to_string(msg->id) << " added." << endl;
}

/**
 * Callback to remove markers from the marker list.
 * Markers are removed by publishing the remove ID to the remove topic.
 */
void onMarkerRemove(const std_msgs::msg::Int32::SharedPtr msg)
{
	for(unsigned int i = 0; i < known.size(); i++)
	{
        if(known[i].id == msg->data)
		{
			known.erase(known.begin() + i);
            cout << "Marker " << to_string(msg->data) << " removed." << endl;
			break;
		}
	}
}

/**
 * Converts a string with numeric values separated by a delimiter to an array of double values.
 * If 0_1_2_3 and delimiter is _ array will contain {0, 1, 2, 3}.
 * @param data String to be converted
 * @param values Array to store values on
 * @param cout Number of elements in the string
 * @param delimiter Separator element
 * @return Array with values.
 */
void stringToDoubleArray(string data, double* values, unsigned int count, string delimiter)
{
	unsigned int pos = 0, k = 0;

	while((pos = data.find(delimiter)) != string::npos && k < count)
	{
		string token = data.substr(0, pos);
		values[k] = stod(token);
		data.erase(0, pos + delimiter.length());
		k++;
	}
}

/**
 * Main method launches aruco ros node, the node gets image and calibration parameters from camera, and publishes position and rotation of the camera relative to the markers.
 * Units should be in meters and radians, the markers are described by a position and an euler rotation.
 * Position is also available as a pose message that should be easier to consume by other ROS nodes.
 * Its possible to pass markers as arugment to this node or register and remove them during runtime using another ROS node.
 * The coordinate system used by OpenCV uses Z+ to represent depth, Y- for height and X+ for lateral, but for the node the coordinate system used is diferent X+ for depth, Z+ for height and Y- for lateral movement.
 * The coordinates are converted on input and on output, its possible to force the OpenCV coordinate system by setting the use_opencv_coords param to true.
 *   
 *           ROS          |          OpenCV
 *    Z+                  |    Y- 
 *    |                   |    |
 *    |    X+             |    |    Z+
 *    |    /              |    |    /
 *    |   /               |    |   /
 *    |  /                |    |  /
 *    | /                 |    | /
 *    |/                  |    |/
 *    O-------------> Y-  |    O-------------> X+
 *
 * @param argc Number of arguments.
 * @param argv Value of the arguments.
 */
int main(int argc, char **argv)
{
    rclcpp::init(argc, argv);

	//ROS node instance
    auto opts = rclcpp::NodeOptions().automatically_declare_parameters_from_overrides(true);
    rclcpp::Node::SharedPtr node = std::make_shared<rclcpp::Node>("maruco", opts);
	
	//Parameters
    node->get_parameter_or<bool>("debug", debug, false);
    node->get_parameter_or<bool>("use_opencv_coords", use_opencv_coords, false);
    node->get_parameter_or<float>("cosine_limit", cosine_limit, 0.7);
    node->get_parameter_or<int>("theshold_block_size_min", theshold_block_size_min, 3);
    node->get_parameter_or<int>("theshold_block_size_max", theshold_block_size_max, 21);
    node->get_parameter_or<float>("max_error_quad", max_error_quad, 0.035);
    node->get_parameter_or<int>("min_area", min_area, 100);
    node->get_parameter_or<bool>("calibrated", calibrated, false);

	//Initial threshold block size
	theshold_block_size = (theshold_block_size_min + theshold_block_size_max) / 2;
	if(theshold_block_size % 2 == 0)
	{
		theshold_block_size++;
	}

	//Initialize calibration matrices
	calibration = Mat(3, 3, CV_64F, data_calibration);
	distortion = Mat(1, 5, CV_64F, data_distortion);

	//Camera instrinsic calibration parameters
    string data;
    node->get_parameter_or<string>("calibration",data,"");
    if(data != "")
	{
		double values[9];
		stringToDoubleArray(data, values, 9, "_");

		for(unsigned int i = 0; i < 9; i++)
		{
			calibration.at<double>(i / 3, i % 3) = values[i];
		}

		calibrated = true;
	}

	//Camera distortion calibration parameters
    node->get_parameter_or<string>("distortion",data,"");
    if(data != "")
	{
		string data;

		double values[5];
		stringToDoubleArray(data, values, 5, "_");

		for(unsigned int i = 0; i < 5; i++)
		{
			distortion.at<double>(0, i) = values[i];
		}

		calibrated = true;
	}

	//Aruco makers passed as parameters
	for(unsigned int i = 0; i < 1024; i++)
	{
        node->get_parameter_or<string>("marker"+to_string(i),data,"");
        if(data != "")
		{
			string data;
            node->get_parameter_or<string>("marker" + to_string(i), data, "1_0_0_0_0_0_0");

			double values[7];
			stringToDoubleArray(data, values, 7, "_");

			//Use OpenCV coordinates
			if(use_opencv_coords)
			{
				known.push_back(ArucoMarkerInfo(i, values[0], Point3d(values[1], values[2], values[3]), Point3d(values[4], values[5], values[6])));
			}
			//Convert coordinates (-Y, -Z, +X)
			else
			{
				known.push_back(ArucoMarkerInfo(i, values[0], Point3d(-values[2], -values[3], -values[1]), Point3d(-values[5], -values[6], values[4])));
			}
		}
	}

	//Print all known markers
	if(debug)
	{
		for(unsigned int i = 0; i < known.size(); i++)
		{
			known[i].print();
		}
	}

	//Subscribed topic names
	string topic_camera, topic_camera_info, topic_marker_register, topic_marker_remove;
    node->get_parameter_or<string>("topic_camera", topic_camera, "/rgb/image");
    node->get_parameter_or<string>("topic_camera_info", topic_camera_info, "/rgb/camera_info");
    node->get_parameter_or<string>("topic_marker_register", topic_marker_register, "/marker_register");
    node->get_parameter_or<string>("topic_marker_remove", topic_marker_remove, "/marker_remove");

	//Publish topic names
	string topic_visible, topic_position, topic_rotation, topic_pose;
    node->get_parameter_or<string>("topic_visible", topic_visible, "/visible");
    node->get_parameter_or<string>("topic_position", topic_position, "/position");
    node->get_parameter_or<string>("topic_rotation", topic_rotation, "/rotation");
    node->get_parameter_or<string>("topic_pose", topic_pose, "/pose");

    std::cout << "camera: " << topic_camera << std::endl << "info: " << topic_camera_info << std::endl
              << "marker_register: " << topic_marker_register << std::endl << "marker_remove:" << topic_marker_remove << std::endl
              << "topic_visible: " << topic_visible << std::endl << "topic_position: " << topic_position << std::endl;

	//Advertise topics
    pub_visible = node->create_publisher<std_msgs::msg::Bool>( topic_visible, rmw_qos_profile_default);
    pub_position = node->create_publisher<geometry_msgs::msg::Point>( topic_position, rmw_qos_profile_default);
    pub_rotation = node->create_publisher<geometry_msgs::msg::Point>(topic_rotation, rmw_qos_profile_default);
    pub_pose = node->create_publisher<geometry_msgs::msg::PoseStamped>( topic_pose, rmw_qos_profile_default);
    //Subscribe topics
    //image_transport::ImageTransport it(node);
    //image_transport::Subscriber sub_camera = it.subscribe(topic_camera, 1, onFrame);
    auto sub_image = node->create_subscription<sensor_msgs::msg::Image>(topic_camera, onFrame, rmw_qos_profile_default);

    auto sub_camera_info = node->create_subscription<sensor_msgs::msg::CameraInfo>(topic_camera_info, onCameraInfo, rmw_qos_profile_default);
    auto sub_marker_register = node->create_subscription<aruco::msg::Marker>(topic_marker_register, onMarkerRegister, rmw_qos_profile_default);
    auto sub_marker_remove = node->create_subscription<std_msgs::msg::Int32>(topic_marker_remove, onMarkerRemove, rmw_qos_profile_default);

    rclcpp::spin(node);

    std::cerr << "Shutdown" << std::endl;
    rclcpp::shutdown();

	return 0;
}
