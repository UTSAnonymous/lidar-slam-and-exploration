#include "brick_search.h"

BrickSearch::BrickSearch(ros::NodeHandle nh, ros::NodeHandle nh_private):it_(nh),nh_(nh),nh_private_(nh_private),tf2_listener_(tf2_buffer_)
{
    ROS_INFO("Brick search started");

    nh_private_.param<std::string>("base_frame", camera_frame_, "camera_rgb_optical_frame");
    nh_private_.param<std::string>("map_frame", map_frame_, "map");

    colorMsg_.subscribe(nh_,"/camera/rgb/image_raw",1);
    depthMsg_.subscribe(nh_,"/camera/depth/image_raw",1);

    //Note, change MySyncPolicy(queueSize) to change how many messages it compares
    sync_.reset(new Sync(MySyncPolicy(QUEUE_SIZE), colorMsg_, depthMsg_));
    //boost::bind uses generic numbers _1, _2, ..., _9 to represent arguments
    //After specifying function, first argument must be an instance of the member function's class
    //That's why 'this' is used because it references an instance of the class
    sync_->registerCallback(boost::bind(&BrickSearch::syncCallBack, this, _1, _2));

    amcl_pose_sub_ = nh_.subscribe("amcl_pose", 1, &BrickSearch::amclPoseCallBack, this);

    pose_pub_ = nh_.advertise<geometry_msgs::PoseStamped>("/move_base_simple/goal", 10);

    // Publishing an image to "/map_image/fbe" topic
    test_image_pub_ = it_.advertise("/map_image/test", 1);
    brick_found_pub_ = nh.advertise<std_msgs::Bool>("/brick_found", 10);

};

BrickSearch::~BrickSearch(){};

void BrickSearch::amclPoseCallBack(const geometry_msgs::PoseWithCovarianceStamped& pose_msg)
{
  
  poseBuffer_.mutex_.lock();
  poseBuffer_.pose = pose_msg.pose.pose;
  poseBuffer_.mutex_.unlock();
  
}

void BrickSearch::syncCallBack(const sensor_msgs::ImageConstPtr& colorMsg, const sensor_msgs::ImageConstPtr& depthMsg)
{   
    //Grab our images from messages
    cv_bridge::CvImagePtr cv_color_ptr;
    cv_bridge::CvImagePtr cv_depth_ptr;

    try
    {
        cv_color_ptr = cv_bridge::toCvCopy(colorMsg, sensor_msgs::image_encodings::BGR8);
        cv_depth_ptr = cv_bridge::toCvCopy(depthMsg, sensor_msgs::image_encodings::TYPE_32FC1);
        //cv_depth_ptr = cv_bridge::toCvCopy(depthMsg);
    }
    catch (cv_bridge::Exception& e)
    {
        ROS_ERROR("cv_bridge exception: %s", e.what());
        return;
    }

    
    //std::cout << cv_depth_ptr->image << std::endl;

    //cv::imshow("teemp",cv_depth_ptr->image);
    //cv::waitKey(1);
    findRedBlob(cv_color_ptr, cv_depth_ptr);
    ROS_INFO_STREAM("brick_found_: " << brick_found_);

};

bool BrickSearch::findRedBlob(const cv_bridge::CvImagePtr& cv_ptr_rgb, const cv_bridge::CvImagePtr& cv_ptr_depth)
{
  // Variables
  cv::Mat image,mask1,mask2,mask3;

  // Convert that frame to seen color
  cv::cvtColor(cv_ptr_rgb->image,image,cv::COLOR_BGR2RGB);

  // Convert to hsv
  cvtColor(cv_ptr_rgb->image, imageHsv_, cv::COLOR_BGR2HSV);
    
  // Creating masks to detect the upper and lower red color.
  cv::inRange(imageHsv_,cv:: Scalar(0, 120, 70),cv::Scalar(10, 255, 255), mask1);
  cv::inRange(imageHsv_,cv::Scalar(170, 120, 70),cv::Scalar(180, 255, 255), mask2);
    
  // Generate the final mask
  mask1 = mask1 + mask2;
  mask3 = mask1;
  cv::bitwise_not(mask1,mask1);
  cv::copyMakeBorder(mask1,mask1,1,1,1,1,cv::BORDER_CONSTANT,255);

  // Setup SimpleBlobDetector parameters.
  cv::SimpleBlobDetector::Params params;

  // Change thresholds
  params.minThreshold = 10;
  params.thresholdStep = 10;
  params.maxThreshold = 220;

  // Filter by Color
  params.filterByColor = true;
  params.blobColor = 0;

  // Filter by Area.
  params.filterByArea = true;
  params.minArea = 1000;
  params.maxArea = 2076601;

  // Filter by Circularity
  params.filterByCircularity = false;

  // Filter by Convexity
  params.filterByConvexity = false;

  // Filter by Inertia
  params.filterByInertia = false;

  // Set up the detector with parameters
  cv::Ptr<cv::SimpleBlobDetector> detector = cv::SimpleBlobDetector::create(params);
    
  // Detect Blobs
  detector->detect(mask1,keypoints_);
  //ROS_INFO_STREAM("Size of keypoints: " << keypoints_.size());
    
  // If keypoints is filled then brick is found
  if (keypoints_.empty()) brick_found_ = false;
  else {
    //calculate waypoint here
    //if(brick_found_ == false) findXYZ(keypoints_[0], cv_ptr_depth);
    findXYZ(keypoints_[0], cv_ptr_depth);

    brick_found_ = true;
    std_msgs::Bool brick_found;
    brick_found.data = true;
    brick_found_pub_.publish(brick_found);
  }

  // Published the blob image on rqt_image_view
  cv::drawKeypoints(image,keypoints_,test_image_, cv::Scalar(0, 0, 255), cv::DrawMatchesFlags::DRAW_RICH_KEYPOINTS);

  sensor_msgs::ImagePtr msg = cv_bridge::CvImage(std_msgs::Header(), "rgb8", test_image_).toImageMsg();
  test_image_pub_.publish(msg);

  // Inform current state
  //ROS_INFO("brickIGotYouInMySight");
};

geometry_msgs::PoseStamped BrickSearch::findXYZ(cv::KeyPoint keypoint, const cv_bridge::CvImagePtr& cv_ptr_depth)
{
    int u = keypoint.pt.x;
    int v = keypoint.pt.y;

    //get depth
    double depth = cv_ptr_depth->image.at<double>(cv::Point(u,v))*100;// * (3000 / 255) + 500;

    // get XYZ using intrinsic parameters of camera
    double X = (depth*((u - PRINCIPAL_POINT_X)/1000)) / (FOCAL_LENGTH_X/1000);
    double Y = (depth*((v - PRINCIPAL_POINT_Y)/1000)) / (FOCAL_LENGTH_Y/1000);

    std::cout << "x :" << X << " y : " << Y << " depth : " << unsigned(depth) << std::endl;

    //get transfrom from map to camera link
    //convert XYZ point from camera to pose
    //multiply the transform together

    tf2::Transform mapToCameraLink;
    fetchTransform(mapToCameraLink, map_frame_, camera_frame_);

    tf2::Transform cameraToBrick;

    tf2::Vector3 origin(X,Y,depth);

    poseBuffer_.mutex_.lock();
    tf2::Quaternion quad(poseBuffer_.pose.orientation.x, 
    poseBuffer_.pose.orientation.y,poseBuffer_.pose.orientation.z, poseBuffer_.pose.orientation.w);
    poseBuffer_.mutex_.unlock();

    cameraToBrick.setOrigin(origin);
    cameraToBrick.setRotation(quad);

    tf2::Transform mapToPose;
    
    mapToPose = mapToCameraLink * cameraToBrick;

    tf2::Vector3 finaltransl = mapToPose.getOrigin();
    tf2::Quaternion finalrot = mapToPose.getRotation();

    geometry_msgs::PoseStamped brick_pose;

    brick_pose.pose.position.x = finaltransl.getX();
    brick_pose.pose.position.y = finaltransl.getY();
    brick_pose.pose.position.z = finaltransl.getZ();

    brick_pose.pose.orientation.x = finalrot.getX();
    brick_pose.pose.orientation.y = finalrot.getY();
    brick_pose.pose.orientation.z = finalrot.getZ();
    brick_pose.pose.orientation.w = finalrot.getW();    

    //std::cout << brick_pose << std::endl;

    return brick_pose;
};

bool BrickSearch::fetchTransform(tf2::Transform &transform, std::string target_frame, std::string source_frame) 
{
    geometry_msgs::TransformStamped local_transformStamped;
    tf2::Stamped<tf2::Transform> tf_stamped;
    try {
        local_transformStamped = tf2_buffer_.lookupTransform(target_frame, source_frame,ros::Time(0));
        tf2::fromMsg(local_transformStamped, tf_stamped);
        transform.setRotation(tf_stamped.getRotation());
        transform.setOrigin(tf_stamped.getOrigin());
        return true;
    }
    catch (tf2::TransformException &ex) {
        ROS_WARN("%s", ex.what());
        return false;
    }
};