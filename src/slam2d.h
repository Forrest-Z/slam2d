#ifndef __SLAM2D_H
#define __SLAM2D_H
#include "slam2d_pose_graph.h"

#include <iostream>
#include <Eigen/Eigen>
#include <sensor_msgs/MultiEchoLaserScan.h>
#include <sensor_msgs/LaserScan.h>
#include <nav_msgs/OccupancyGrid.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/visualization/cloud_viewer.h>
#include <opencv2/opencv.hpp>


using namespace std;
using namespace Eigen;
using namespace cv;


typedef pcl::PointXY PointType;

typedef struct
{
    double theta;
    Eigen::Vector2d t;

} state2d;

Eigen::Vector2d point2eigen(PointType p)
{
    Eigen::Vector2d pp;
    pp(0) = p.x;
    pp(1) = p.y;
    return pp;
}

PointType eigen2point(Eigen::Vector2d pp)
{
    PointType p;
    p.x = pp(0);
    p.y = pp(1);
    return p;
}

class slam2d
{
private:
    /* data */
public:
    slam2d(/* args */);
    ~slam2d();
    state2d state;
    state2d delta;
    double timestamp;
    nav_msgs::OccupancyGrid map2d;
    Mat cvmap2d;

    pcl::PointCloud<PointType> scan;
    pcl::PointCloud<PointType> scan_prev;

    pcl::PointCloud<PointType> scan_normal;
    bool cvmap_vis_enable = false;


    void readin_scan_data(const sensor_msgs::MultiEchoLaserScanConstPtr &msg);
    void readin_scan_data(const sensor_msgs::LaserScanConstPtr &msg);

    void update_scan_normal();
    void scan_match();
    void update();
    void update_transform();
    void update_map();
    void cvmap2map();//convert cv map to map
};

slam2d::slam2d()
{
    state.t = Vector2d::Zero();
    state.theta = 0;
    map2d.header.frame_id = "odom";
    map2d.info.width = 2000;
    map2d.info.height = 2000;
    map2d.info.resolution = 0.1;
    map2d.info.origin.orientation.w = 1;
    map2d.info.origin.orientation.x = 0;
    map2d.info.origin.orientation.y = 0;
    map2d.info.origin.orientation.z = 0;
    map2d.info.origin.position.x = -0.5 * map2d.info.width * map2d.info.resolution;
    map2d.info.origin.position.y = -0.5 * map2d.info.height * map2d.info.resolution;
    map2d.info.origin.position.z = 0;
    map2d.data.resize(map2d.info.width * map2d.info.height);
    cvmap2d = Mat(map2d.info.width, map2d.info.height, CV_8SC1, -1);
    cvmap2map();
}

slam2d::~slam2d()
{
}

void slam2d::readin_scan_data(const sensor_msgs::MultiEchoLaserScanConstPtr &msg)
{
    timestamp = msg->header.stamp.toSec();
    scan.points.resize(msg->ranges.size());
    for (auto i = 0; i < msg->ranges.size(); i++)
    {
        float dist = msg->ranges[i].echoes[0]; //only first echo used for slam2d
        float theta = msg->angle_min + i * msg->angle_increment;
        scan.points[i].x = dist * cos(theta);
        scan.points[i].y = dist * sin(theta);
    }
    scan.width = scan.points.size();
    scan.height = 1;
    scan.is_dense = true;
}
void slam2d::readin_scan_data(const sensor_msgs::LaserScanConstPtr &msg)
{
    timestamp = msg->header.stamp.toSec();
    scan.points.resize(msg->ranges.size());
    for (auto i = 0; i < msg->ranges.size(); i++)
    {
        float dist = msg->ranges[i]; //only first echo used for slam2d
        float theta = msg->angle_min + i * msg->angle_increment;
        scan.points[i].x = dist * cos(theta);
        scan.points[i].y = dist * sin(theta);
    }
    scan.width = scan.points.size();
    scan.height = 1;
    scan.is_dense = true;
}

void slam2d::update_scan_normal()
{
    //compute normal of scan
    scan_normal.points.resize(scan.points.size());
    for (auto i = 1; i < scan.points.size(); i++)
    {
        PointType p1 = scan.points[i - 1];
        PointType p2 = scan.points[i];

        Eigen::Vector2d delta(p1.x - p2.x, p1.y - p2.y);
        Eigen::Vector2d normal(-delta(1), delta(0));
        normal /= normal.norm();
        scan_normal.points[i].x = normal(0);
        scan_normal.points[i].y = normal(1);
    }
}
void slam2d::scan_match()
{
    double pose[3] = {0};
    if (scan.points.size() && scan_prev.points.size())
    {

        Problem problem;
        //solve delta with ceres constraints
        pcl::KdTreeFLANN<PointType> kdtree;
        kdtree.setInputCloud(scan.makeShared());
        int K = 2; // K nearest neighbor search
        std::vector<int> index(K);
        std::vector<float> distance(K);
        //1. project scan_prev to scan

        Eigen::Matrix2d R;
        R(0, 0) = cos(delta.theta); R(0, 1) = -sin(delta.theta);
        R(1, 0) = sin(delta.theta); R(1, 1) = cos(delta.theta);
        Eigen::Vector2d dt = delta.t;
        //find nearest neighur
        for (int i = 0; i < scan_prev.points.size(); i++)
        {
            PointType search_point = scan_prev.points[i];
            //project search_point to current frame
            PointType search_point_predict = eigen2point(R * point2eigen(search_point) + dt);
            if (kdtree.nearestKSearch(search_point_predict, K, index, distance) == K)
            {
                //add constraints
                Eigen::Vector2d p = point2eigen(search_point);
                Eigen::Vector2d p1 = point2eigen(scan.points[index[0]]);
                Eigen::Vector2d p2 = point2eigen(scan.points[index[1]]);
                ceres::CostFunction *cost_function = lidar_edge_error::Create(p, p1, p2);
                problem.AddResidualBlock(cost_function,
                                         new CauchyLoss(0.5),
                                         pose);
            }
        }

        ceres::Solver::Options options;
        options.linear_solver_type = ceres::DENSE_SCHUR;
        options.minimizer_progress_to_stdout = false;

        ceres::Solver::Summary summary;
        ceres::Solve(options, &problem, &summary);
        std::cout << summary.FullReport() << "\n";

        printf("result: %lf, %lf, %lf\n", pose[0], pose[1], pose[2]);

        delta.theta = pose[0];
        delta.t(0) = pose[1];
        delta.t(1) = pose[2];

    }
}

void slam2d::update_transform()
{

    Eigen::Matrix2d dR;
    dR(0, 0) = cos(delta.theta); dR(0, 1) = -sin(delta.theta);
    dR(1, 0) = sin(delta.theta); dR(1, 1) = cos(delta.theta);


    Eigen::Vector2d dt_inv = -dR.transpose() * delta.t;
    Eigen::Matrix2d dR_inv = dR.transpose();
    

    Eigen::Matrix2d R;
    R(0, 0) = cos(state.theta); R(0, 1) = -sin(state.theta);
    R(1, 0) = sin(state.theta); R(1, 1) =  cos(state.theta);
    state.theta += (-delta.theta);
    state.t += R * dt_inv;
}

void slam2d::update()
{
    static int cnt = 0;
    if (scan.points.size() && scan_prev.points.size())
    {
        scan_match();
        update_transform();
        if (cnt % 10 == 0)
        {
            update_map();
        }

    }

    if (scan.points.size())
    {
        scan_prev = scan;
    }
    cnt++;
}

void slam2d::update_map()
{
    //update map with scan and state
    int offsetx = cvmap2d.cols / 2;
    int offsety = cvmap2d.rows / 2;
    cv::Point2f origin(state.t(0) / map2d.info.resolution + offsetx, state.t(1) / map2d.info.resolution + offsety);
    Eigen::Matrix2d R;
    R(0, 0) = cos(state.theta); R(0, 1) = -sin(state.theta);
    R(1, 0) = sin(state.theta); R(1, 1) =  cos(state.theta);
    for (int i = 0; i < scan.points.size(); i++)
    {
        PointType p = scan.points[i];
        float dist = sqrtf(p.x * p.x + p.y * p.y);
        if (dist > 10) continue;
        Eigen::Vector2d pp = R * point2eigen(p) + state.t;
        cv:Point2f ppp(pp(0)/ map2d.info.resolution + offsetx, pp(1)/ map2d.info.resolution + offsety);
        
        cv::line(cvmap2d, origin, ppp, 100, 1, 4, 0);
        cv::circle(cvmap2d, ppp, 1, 0, 1, 4, 0);
    }
    cvmap2map();
}

void slam2d::cvmap2map()
{
    for (int i = 0; i < cvmap2d.rows; i++)
    {
        for(int j = 0; j < cvmap2d.cols; j++)
        {
            map2d.data[i * map2d.info.width + j] = cvmap2d.at<int8_t>(i, j);
        }
    }
    if (cvmap_vis_enable)
    {
        imshow("cvmap2d", cvmap2d);
        waitKey(2);
    }
}
#endif
