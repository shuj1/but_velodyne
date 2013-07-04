/******************************************************************************
 * \file
 *
 * $Id:$
 *
 * Copyright (C) Brno University of Technology (BUT)
 *
 * This file is part of software developed by Robo@FIT group.
 *
 * Author: Michal Spanel (spanel@fit.vutbr.cz)
 * Supervised by: Michal Spanel (spanel@fit.vutbr.cz)
 * Date: 28/06/2013
 *
 * This file is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <but_velodyne/laser_scan.h>
#include <but_velodyne/parameters_list.h>
#include <but_velodyne/topics_list.h>

#include <cmath>
#include <opencv2/core/core.hpp>

namespace but_velodyne
{

/******************************************************************************
 */

LaserScan::LaserScan(ros::NodeHandle nh, ros::NodeHandle private_nh)
    : nh_(nh)
    , private_nh_(private_nh)
{
    // Load parameters
    private_nh_.param( FRAME_ID_PARAM, params_.frame_id, params_.frame_id );
    private_nh_.param( MIN_Z_PARAM, params_.min_z, params_.min_z );
    private_nh_.param( MAX_Z_PARAM, params_.max_z, params_.max_z );
    private_nh_.param( ANGULAR_RES_PARAM, params_.angular_res, params_.angular_res );

    // If a tf_prefix param is specified, it will be added to the beginning of the frame ID
//    std::string tf_prefix = tf::getPrefixParam( private_nh_ );
//    if( !tf_prefix.empty() )
//    {
//        params_.frame_id = tf::resolve( tf_prefix, params_.frame_id);
//    }

    ROS_INFO_STREAM( FRAME_ID_PARAM << " parameter: " << params_.frame_id );
    ROS_INFO_STREAM( MIN_Z_PARAM << " parameter: " << params_.min_z );
    ROS_INFO_STREAM( MAX_Z_PARAM << " parameter: " << params_.max_z );
    ROS_INFO_STREAM( ANGULAR_RES_PARAM << " parameter: " << params_.angular_res );

    // Advertise output laser scan
    scan_pub_ = nh_.advertise<sensor_msgs::LaserScan>( OUTPUT_LASER_SCAN_TOPIC, 10 );

    // Subscribe to Velodyne point cloud
    if( params_.frame_id.empty() )
    {
        // No TF frame ID conversion required
        points_sub_ = nh_.subscribe<sensor_msgs::PointCloud2>(INPUT_POINT_CLOUD_TOPIC, 1, &LaserScan::process, this );
    }
    else
    {
//        points_sub_filtered_.subscribe(nh_, INPUT_POINT_CLOUD_TOPIC, 10);
        points_sub_filtered_.subscribe(nh_, INPUT_POINT_CLOUD_TOPIC, 10);
        tf_filter_ = new tf::MessageFilter<sensor_msgs::PointCloud2>( points_sub_filtered_, listener_, params_.frame_id, 10 );
        tf_filter_->registerCallback( boost::bind(&LaserScan::process, this, _1) );
    }
}


/******************************************************************************
 */
void LaserScan::process(const sensor_msgs::PointCloud2::ConstPtr &cloud)
{
    ROS_INFO_STREAM_ONCE( "Point cloud received" );

    if( scan_pub_.getNumSubscribers() == 0 )
    {
        return;
    }

    // Retrieve the input point cloud
    pcl::fromROSMsg( *cloud, pcl_in_ );

    // Copy message header
    scan_out_.header.stamp = cloud->header.stamp;

    // Target TF frame ID
    if( params_.frame_id.empty() )
    {
        // No TF transformation required
        scan_out_.header.frame_id = cloud->header.frame_id;
    }
    else
    {
        // Prescribed frame ID
        scan_out_.header.frame_id = params_.frame_id;
        if( scan_out_.header.frame_id != cloud->header.frame_id )
        {
            // Get TF transform
            tf::StampedTransform to_target_frame_tf;
            try
            {
                ROS_INFO_STREAM_ONCE( "Transforming point cloud from " << cloud->header.frame_id
                                << " to " << scan_out_.header.frame_id
                                );
                pcl_ros::transformPointCloud( scan_out_.header.frame_id, pcl_in_, pcl_in_, listener_ );
            }
            catch( tf::TransformException ex )
            {
                ROS_INFO_STREAM_ONCE( "Cannot transform the point cloud!" );
                return;
            }
        }
    }

    // Calculate the number of output bins
    int num_of_bins = ( params_.angular_res > 0.0f ) ? int(360 / params_.angular_res) : 4096;
    float angular_res = 360.0f / num_of_bins;
    float inv_angular_res = 1.0f / angular_res;
    float rad_to_deg = 180.0f / float(CV_PI);
    float range_min = 1e7f, range_max = -1e7f;

    // Initialize the simulated laser scan
    scan_out_.ranges.resize(num_of_bins);
    scan_out_.intensities.resize(num_of_bins);
    for( int i = 0; i < num_of_bins; ++i )
    {
        scan_out_.ranges[i] = range_min;
        scan_out_.intensities[i] = 0.0f;
    }

    // Create the simulated laser scan
    VPointCloud::iterator itEnd = pcl_in_.end();
    for( VPointCloud::iterator it = pcl_in_.begin(); it != itEnd; ++it )
    {
//        ROS_INFO_STREAM("Point: " << it->x << ", " << it->y << ", " << it->z);

        // Check the point
        if( params_.min_z != params_.max_z )
        {
            if( it->z < params_.min_z || it->z > params_.max_z )
                continue;
        }

        // Conversion to the polar coordinates
        float mag = std::sqrt(it->x * it->x + it->y * it->y);
        float ang = std::atan2(it->y, it->x) * rad_to_deg;
//        float mag = cv::sqrt(it->x * it->x + it->y * it->y);
//        float ang = cv::fastAtan2(it->y, it->x); // precision ~0.3 degrees

//        ROS_INFO_STREAM("Polar coords: " << mag << ", " << ang);

        // Find the corresponding bin
        int n = (ang + 180.0f) * inv_angular_res;
        if( n >= num_of_bins )
            n = num_of_bins - 1;
        else if( n < 0 )
            n = 0;

//        ROS_INFO_STREAM("Bin num.: " << n);

        // Accumulate the value
        if( mag < scan_out_.ranges[n] )
        {
            scan_out_.ranges[n] = mag;
            scan_out_.intensities[n] = it->intensity;
        }

        // Overall stats
        range_min = (mag < range_min) ? mag : range_min;
        range_max = (mag > range_max) ? mag : range_max;
    }

    // Fill in all message members
    scan_out_.angle_min = -float(CV_PI);
    scan_out_.angle_max = float(CV_PI);
    scan_out_.angle_increment = angular_res / rad_to_deg;
    scan_out_.range_min = range_min;
    scan_out_.range_max = range_max;
    scan_out_.scan_time = 60 / 10;      // TODO: get the value from Velodyne, fixed to 10Hz for now
    scan_out_.time_increment = scan_out_.scan_time / float(num_of_bins);

    // Publish the accumulated laser scan
    ROS_INFO_STREAM_ONCE( "Publishing laser scan " << scan_out_.header.stamp );

    scan_pub_.publish(scan_out_);
}


} // namespace velodyne_pointcloud
