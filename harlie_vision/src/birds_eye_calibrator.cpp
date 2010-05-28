#include <ros/ros.h>
#include <image_transport/image_transport.h>
#include <opencv/cv.h>
#include <opencv/highgui.h>
#include <cv_bridge/CvBridge.h>
#include <std_srvs/Empty.h>
#include <vector>
#include <iostream>

class BirdsEyeCalibrator {
	public:
		BirdsEyeCalibrator();
	private:
		void image_callback(const sensor_msgs::ImageConstPtr& msg);

		int32_t board_width;
		int32_t board_height;
		double m_per_output_pixel;
		double square_width;

		ros::NodeHandle nh_;
		image_transport::ImageTransport it_;
		image_transport::Subscriber image_subscriber;
		image_transport::Publisher image_publisher;
};

BirdsEyeCalibrator::BirdsEyeCalibrator() : it_(nh_) {
	nh_.param("board_height", board_height, 6);
	nh_.param("board_width", board_width, 8);
	nh_.param("square_width", square_width, 0.02858);
	nh_.param("cm_per_output_pixel", m_per_output_pixel, 0.01);

	image_subscriber = it_.subscribe("image_rect", 1, &BirdsEyeCalibrator::image_callback, this);
	image_publisher = it_.advertise("plan_view_sample", 1);
}

void BirdsEyeCalibrator::image_callback(const sensor_msgs::ImageConstPtr& msg) {
	sensor_msgs::CvBridge bridge;
	cv::Mat image;
	cv::Mat gray_image;
	cv::Mat output;
	try
	{
		image = cv::Mat(bridge.imgMsgToCv(msg, "bgr8"));

	}
	catch (sensor_msgs::CvBridgeException& e)
	{
		ROS_ERROR("Could not convert from '%s' to 'bgr8'.", msg->encoding.c_str());
	}
	cv::cvtColor(image, gray_image, CV_BGR2GRAY);	
	cv::Size board_size = cv::Size(this->board_width, this->board_height);
	std::vector<cv::Point2f> corners;		
	bool found = cv::findChessboardCorners(gray_image, board_size, corners, CV_CALIB_CB_ADAPTIVE_THRESH | CV_CALIB_CB_FILTER_QUADS);
	if (!found) {
	    //Not found... currently, we do nothing
	    std::cout << "No corners found" << std::endl;
	    return;
	}
	cornerSubPix(gray_image, corners, cv::Size(11, 11), cv::Size(-1,-1), cv::TermCriteria(CV_TERMCRIT_EPS | CV_TERMCRIT_ITER, 30, 0.1));
	std::vector<cv::Point2f> objPts;
	std::vector<cv::Point2f> imgPts;
	objPts.push_back(cv::Point2f(0, 0));
	//objPts.push_back(cv::Point2f(this->grid_width*(this->board_width-1), 0));
	//objPts.push_back(cv::Point2f(0, this->grid_width*(this->board_height-1)));
	//objPts.push_back(cv::Point2f(this->grid_width*(this->board_width-1), this->grid_width*(this->board_height-1)));
	imgPts.push_back(corners[0]);
	imgPts.push_back(corners[(this->board_width-1)]);
	imgPts.push_back(corners[(this->board_height-1)*this->board_width]);
	imgPts.push_back(corners[(this->board_height-1)*this->board_width + (this->board_width-1)]);
/*	for(uint32_t i = 0; i < corners.size(); i++) {	 
		cv::circle(image, corners[i], 9, CV_RGB(0,0,255), 3);
		cv::imshow("view", image);
		cv::waitKey();
	} */
	cv::circle(image, imgPts[0], 9, CV_RGB(0,0,255), 3);
	cv::imshow("view", image);
}

int main(int argc, char* argv[]) {
	ros::init(argc, argv, "birds_eye_calibrator");
	BirdsEyeCalibrator calibrator;
	cvNamedWindow("view");
	cvStartWindowThread();
	ros::spin();
	cvDestroyWindow("view");
}
