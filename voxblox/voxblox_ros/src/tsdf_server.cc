#include "voxblox_ros/tsdf_server.h"

#include <geometry_msgs/PoseStamped.h>
#include <minkindr_conversions/kindr_msg.h>
#include <minkindr_conversions/kindr_tf.h>
#include <voxblox/integrator/projective_tsdf_integrator.h>
#include <voxblox_msgs/LayerWithTrajectory.h>

#include "voxblox_ros/conversions.h"
#include "voxblox_ros/ros_params.h"

namespace voxblox {

TsdfServer::TsdfServer(const ros::NodeHandle& nh,
                       const ros::NodeHandle& nh_private)
    : TsdfServer(nh, nh_private, getTsdfMapConfigFromRosParam(nh_private),
                 getTsdfIntegratorConfigFromRosParam(nh_private),
                 getMeshIntegratorConfigFromRosParam(nh_private)) {}

TsdfServer::TsdfServer(const ros::NodeHandle& nh,
                       const ros::NodeHandle& nh_private,
                       const TsdfMap::Config& config,
                       const TsdfIntegratorBase::Config& integrator_config,
                       const MeshIntegratorConfig& mesh_config)
    : nh_(nh),
      nh_private_(nh_private),
      verbose_(true),
      world_frame_("world"),
      icp_corrected_frame_("icp_corrected"),
      pose_corrected_frame_("pose_corrected"),
      max_block_distance_from_body_(std::numeric_limits<FloatingPoint>::max()),
      slice_level_(0.5),
      slice_level_follow_robot_(false),
      use_freespace_pointcloud_(false),
      color_map_(new RainbowColorMap()),
      publish_pointclouds_on_update_(false),
      publish_slices_(false),
      publish_pointclouds_(false),
      publish_tsdf_map_(false),
      cache_mesh_(false),
      enable_icp_(false),
      accumulate_icp_corrections_(true),
      pointcloud_queue_size_(1),
      num_subscribers_tsdf_map_(0),
      transformer_(nh, nh_private),
      pointcloud_deintegration_queue_length_(0),
      num_voxels_per_block_(config.tsdf_voxels_per_side *
                            config.tsdf_voxels_per_side *
                            config.tsdf_voxels_per_side),
      map_needs_pruning_(false),
      publish_map_with_trajectory_(false),
      num_subscribers_active_tsdf_(0),
      map_running_(true),
      submap_interval_(0.0) {
  getServerConfigFromRosParam(nh_private);

  // Advertise topics.
  surface_pointcloud_pub_ =
      nh_private_.advertise<pcl::PointCloud<pcl::PointXYZRGB>>(
          "surface_pointcloud", 1, true);
  tsdf_pointcloud_pub_ = nh_private_.advertise<pcl::PointCloud<pcl::PointXYZI>>(
      "tsdf_pointcloud", 1, true);
  occupancy_marker_pub_ =
      nh_private_.advertise<visualization_msgs::MarkerArray>("occupied_nodes",
                                                             1, true);
  tsdf_slice_pub_ = nh_private_.advertise<pcl::PointCloud<pcl::PointXYZI>>(
      "tsdf_slice", 1, true);
  reprojected_pointcloud_pub_ =
      nh_private_.advertise<pcl::PointCloud<pcl::PointXYZ>>(
          "reprojected_pointcloud", 1, false);

  nh_private_.param("pointcloud_queue_size", pointcloud_queue_size_,
                    pointcloud_queue_size_);
  pointcloud_sub_ = nh_.subscribe("pointcloud", pointcloud_queue_size_,
                                  &TsdfServer::insertPointcloud, this);

  mesh_pub_ = nh_private_.advertise<voxblox_msgs::Mesh>("mesh", 1, true);

  nh_private_.param("publish_mesh_with_history", publish_mesh_with_history_,
                    publish_mesh_with_history_);

  LOG(INFO) << publish_mesh_with_history_;
  if (publish_mesh_with_history_)
    mesh_with_history_pub_ = nh_private_.advertise<voxblox_msgs::Mesh>(
        "mesh_with_history", 10, true);

  // Publishing/subscribing to a layer from another node (when using this as
  // a library, for example within a planner).
  if (publish_map_with_trajectory_) {
    tsdf_map_pub_ = nh_private_.advertise<voxblox_msgs::LayerWithTrajectory>(
        "tsdf_map_out", 1, false);
  } else {
    tsdf_map_pub_ =
        nh_private_.advertise<voxblox_msgs::Layer>("tsdf_map_out", 1, false);
  }
  tsdf_map_sub_ = nh_private_.subscribe("tsdf_map_in", 1,
                                        &TsdfServer::tsdfMapCallback, this);
  nh_private_.param("publish_tsdf_map", publish_tsdf_map_, publish_tsdf_map_);

  if (use_freespace_pointcloud_) {
    // points that are not inside an object, but may also not be on a surface.
    // These will only be used to mark freespace beyond the truncation distance.
    freespace_pointcloud_sub_ =
        nh_.subscribe("freespace_pointcloud", pointcloud_queue_size_,
                      &TsdfServer::insertFreespacePointcloud, this);
  }

  if (enable_icp_) {
    icp_transform_pub_ = nh_private_.advertise<geometry_msgs::TransformStamped>(
        "icp_transform", 1, true);
    nh_private_.param("icp_corrected_frame", icp_corrected_frame_,
                      icp_corrected_frame_);
    nh_private_.param("pose_corrected_frame", pose_corrected_frame_,
                      pose_corrected_frame_);
  }

  // Initialize TSDF Map and integrator.
  tsdf_map_.reset(new TsdfMap(config));

  std::string method("merged");
  nh_private_.param("method", method, method);
  tsdf_integrator_ = TsdfIntegratorFactory::create(
      method, integrator_config, tsdf_map_->getTsdfLayerPtr());

  mesh_layer_.reset(new MeshLayer(tsdf_map_->block_size()));

  mesh_integrator_.reset(new MeshIntegrator<TsdfVoxel>(
      mesh_config, tsdf_map_->getTsdfLayerPtr(), mesh_layer_.get()));

  icp_.reset(new ICP(getICPConfigFromRosParam(nh_private)));

  // Advertise services.
  generate_mesh_srv_ = nh_private_.advertiseService(
      "generate_mesh", &TsdfServer::generateMeshCallback, this);
  clear_map_srv_ = nh_private_.advertiseService(
      "clear_map", &TsdfServer::clearMapCallback, this);
  save_map_srv_ = nh_private_.advertiseService(
      "save_map", &TsdfServer::saveMapCallback, this);
  load_map_srv_ = nh_private_.advertiseService(
      "load_map", &TsdfServer::loadMapCallback, this);
  publish_pointclouds_srv_ = nh_private_.advertiseService(
      "publish_pointclouds", &TsdfServer::publishPointcloudsCallback, this);
  publish_tsdf_map_srv_ = nh_private_.advertiseService(
      "publish_map", &TsdfServer::publishTsdfMapCallback, this);

  // If set, use a timer to progressively integrate the mesh.
  double update_mesh_every_n_sec = 1.0;
  nh_private_.param("update_mesh_every_n_sec", update_mesh_every_n_sec,
                    update_mesh_every_n_sec);

  if (update_mesh_every_n_sec > 0.0) {
    update_mesh_timer_ =
        nh_private_.createTimer(ros::Duration(update_mesh_every_n_sec),
                                &TsdfServer::updateMeshEvent, this);
  }

  double publish_map_every_n_sec = 1.0;
  nh_private_.param("publish_map_every_n_sec", publish_map_every_n_sec,
                    publish_map_every_n_sec);

  nh_private_.param("submap_interval", submap_interval_, submap_interval_);

  if (submap_interval_ > 0.0) {
    CHECK_EQ(pointcloud_deintegration_queue_length_, 0);
    CHECK_EQ(publish_map_every_n_sec, 0.0);
  }

  if (publish_map_every_n_sec > 0.0) {
    publish_map_timer_ =
        nh_private_.createTimer(ros::Duration(publish_map_every_n_sec),
                                &TsdfServer::publishMapEvent, this);
  }

  nh_private_.param<float>("publish_active_tsdf_every_n_sec",
                           publish_active_tsdf_every_n_sec_, 0.0);

  if (publish_active_tsdf_every_n_sec_ > 0.0) {
    active_tsdf_pub_ =
        nh_private_.advertise<voxblox_msgs::Layer>("active_tsdf_out", 10, true);
    active_map_pub_timer_ =
        nh_private_.createTimer(ros::Duration(publish_active_tsdf_every_n_sec_),
                                &TsdfServer::activeMapPubCallback, this);
  }

  toggle_mapping_srv_ = nh_private_.advertiseService(
      "toggle_mapping", &TsdfServer::toogleMappingCallback, this);

  nh_private_.param<int>("max_gap", max_gap_, 4);
  nh_private_.param<int>("min_n", min_n_, 2);

  mesh_histroy_config_ = getMeshIntegratorConfigFromRosParam(nh_private_);

  if (submap_interval_ > 0) {
    create_new_submap_timer_ =
        nh_private_.createTimer(ros::Duration(submap_interval_),
                                &TsdfServer::createNewSubmapEvent, this);
  }
}

void TsdfServer::getServerConfigFromRosParam(
    const ros::NodeHandle& nh_private) {
  // Before subscribing, determine minimum time between messages.
  // 0 by default.
  double min_time_between_msgs_sec = 0.0;
  nh_private.param("min_time_between_msgs_sec", min_time_between_msgs_sec,
                   min_time_between_msgs_sec);
  min_time_between_msgs_.fromSec(min_time_between_msgs_sec);

  nh_private.param("max_block_distance_from_body",
                   max_block_distance_from_body_,
                   max_block_distance_from_body_);
  nh_private.param("slice_level", slice_level_, slice_level_);
  nh_private.param("slice_level_follow_robot", slice_level_follow_robot_,
                   slice_level_follow_robot_);
  nh_private.param("world_frame", world_frame_, world_frame_);
  nh_private.param("publish_pointclouds_on_update",
                   publish_pointclouds_on_update_,
                   publish_pointclouds_on_update_);
  nh_private.param("publish_slices", publish_slices_, publish_slices_);
  nh_private.param("publish_pointclouds", publish_pointclouds_,
                   publish_pointclouds_);

  nh_private.param("use_freespace_pointcloud", use_freespace_pointcloud_,
                   use_freespace_pointcloud_);
  nh_private.param("pointcloud_queue_size", pointcloud_queue_size_,
                   pointcloud_queue_size_);
  nh_private.param("enable_icp", enable_icp_, enable_icp_);
  nh_private.param("accumulate_icp_corrections", accumulate_icp_corrections_,
                   accumulate_icp_corrections_);

  nh_private.param<std::string>("pointcloud_frame", pointcloud_frame_, "");
  nh_private.param("verbose", verbose_, verbose_);

  // Pointcloud deintegration settings
  {
    int pointcloud_deintegration_queue_length_int;
    if (nh_private.getParam("pointcloud_deintegration_queue_length",
                            pointcloud_deintegration_queue_length_int)) {
      pointcloud_deintegration_queue_length_ =
          pointcloud_deintegration_queue_length_int;
    }
  }
  nh_private.param("publish_map_with_trajectory", publish_map_with_trajectory_,
                   publish_map_with_trajectory_);

  // Mesh settings.
  nh_private.param("mesh_filename", mesh_filename_, mesh_filename_);
  std::string color_mode("");
  nh_private.param("color_mode", color_mode, color_mode);
  color_mode_ = getColorModeFromString(color_mode);

  // Color map for intensity pointclouds.
  std::string intensity_colormap("rainbow");
  float intensity_max_value = kDefaultMaxIntensity;
  nh_private.param("intensity_colormap", intensity_colormap,
                   intensity_colormap);
  nh_private.param("intensity_max_value", intensity_max_value,
                   intensity_max_value);

  // Default set in constructor.
  if (intensity_colormap == "rainbow") {
    color_map_.reset(new RainbowColorMap());
  } else if (intensity_colormap == "inverse_rainbow") {
    color_map_.reset(new InverseRainbowColorMap());
  } else if (intensity_colormap == "grayscale") {
    color_map_.reset(new GrayscaleColorMap());
  } else if (intensity_colormap == "inverse_grayscale") {
    color_map_.reset(new InverseGrayscaleColorMap());
  } else if (intensity_colormap == "ironbow") {
    color_map_.reset(new IronbowColorMap());
  } else {
    ROS_ERROR_STREAM("Invalid color map: " << intensity_colormap);
  }
  color_map_->setMaxValue(intensity_max_value);
}

void TsdfServer::processPointCloudMessageAndInsert(
    const sensor_msgs::PointCloud2::Ptr& pointcloud_msg,
    const Transformation& T_G_C, const bool is_freespace_pointcloud) {
  // Convert the PCL pointcloud into our awesome format.

  // Horrible hack fix to fix color parsing colors in PCL.
  bool color_pointcloud = false;
  bool has_intensity = false;
  for (size_t d = 0; d < pointcloud_msg->fields.size(); ++d) {
    if (pointcloud_msg->fields[d].name == std::string("rgb")) {
      pointcloud_msg->fields[d].datatype = sensor_msgs::PointField::FLOAT32;
      color_pointcloud = true;
    } else if (pointcloud_msg->fields[d].name == std::string("intensity")) {
      has_intensity = true;
    }
  }

  auto points_C = std::make_shared<Pointcloud>();
  auto colors = std::make_shared<Colors>();
  timing::Timer ptcloud_timer("ptcloud_preprocess");

  // Convert differently depending on RGB or I type.
  if (color_pointcloud) {
    pcl::PointCloud<pcl::PointXYZRGB> pointcloud_pcl;
    // pointcloud_pcl is modified below:
    pcl::fromROSMsg(*pointcloud_msg, pointcloud_pcl);
    convertPointcloud(pointcloud_pcl, color_map_, points_C.get(), colors.get());
  } else if (has_intensity) {
    pcl::PointCloud<pcl::PointXYZI> pointcloud_pcl;
    // pointcloud_pcl is modified below:
    pcl::fromROSMsg(*pointcloud_msg, pointcloud_pcl);
    convertPointcloud(pointcloud_pcl, color_map_, points_C.get(), colors.get());
  } else {
    pcl::PointCloud<pcl::PointXYZ> pointcloud_pcl;
    // pointcloud_pcl is modified below:
    pcl::fromROSMsg(*pointcloud_msg, pointcloud_pcl);
    convertPointcloud(pointcloud_pcl, color_map_, points_C.get(), colors.get());
  }
  ptcloud_timer.Stop();

  Transformation T_G_C_refined = T_G_C;
  if (enable_icp_) {
    timing::Timer icp_timer("icp");
    if (!accumulate_icp_corrections_) {
      icp_corrected_transform_.setIdentity();
    }
    static Transformation T_offset;
    const size_t num_icp_updates =
        icp_->runICP(tsdf_map_->getTsdfLayer(), *points_C,
                     icp_corrected_transform_ * T_G_C, &T_G_C_refined);
    if (verbose_) {
      ROS_INFO("ICP refinement performed %zu successful update steps",
               num_icp_updates);
    }
    icp_corrected_transform_ = T_G_C_refined * T_G_C.inverse();

    if (!icp_->refiningRollPitch()) {
      // its already removed internally but small floating point errors can
      // build up if accumulating transforms
      Transformation::Vector6 T_vec = icp_corrected_transform_.log();
      T_vec[3] = 0.0;
      T_vec[4] = 0.0;
      icp_corrected_transform_ = Transformation::exp(T_vec);
    }

    // Publish transforms as both TF and message.
    tf::Transform icp_tf_msg, pose_tf_msg;
    geometry_msgs::TransformStamped transform_msg;

    tf::transformKindrToTF(icp_corrected_transform_.cast<double>(),
                           &icp_tf_msg);
    tf::transformKindrToTF(T_G_C.cast<double>(), &pose_tf_msg);
    tf::transformKindrToMsg(icp_corrected_transform_.cast<double>(),
                            &transform_msg.transform);
    tf_broadcaster_.sendTransform(
        tf::StampedTransform(icp_tf_msg, pointcloud_msg->header.stamp,
                             world_frame_, icp_corrected_frame_));
    tf_broadcaster_.sendTransform(
        tf::StampedTransform(pose_tf_msg, pointcloud_msg->header.stamp,
                             icp_corrected_frame_, pose_corrected_frame_));

    transform_msg.header.frame_id = world_frame_;
    transform_msg.child_frame_id = icp_corrected_frame_;
    icp_transform_pub_.publish(transform_msg);

    icp_timer.Stop();
  }

  // Integrate the new pointcloud
  if (verbose_) {
    ROS_INFO("Integrating a pointcloud with %lu points.", points_C->size());
  }
  ros::WallTime start_integration = ros::WallTime::now();
  integratePointcloud(pointcloud_msg->header.stamp, T_G_C_refined, points_C,
                      colors, is_freespace_pointcloud);
  ros::WallTime end_integration = ros::WallTime::now();
  if (verbose_) {
    ROS_INFO("Finished integrating in %f seconds, have %lu blocks.",
             (end_integration - start_integration).toSec(),
             tsdf_map_->getTsdfLayer().getNumberOfAllocatedBlocks());
  }

  // Visualize the reprojected pointcloud, usually for debugging purposes
  if (reprojected_pointcloud_pub_.getNumSubscribers() > 0) {
    auto projective_integrator_ptr =
        dynamic_cast<voxblox::ProjectiveTsdfIntegrator<
            voxblox::InterpolationScheme::kAdaptive>*>(tsdf_integrator_.get());
    if (projective_integrator_ptr) {
      const voxblox::Pointcloud reprojected_pointcloud =
          projective_integrator_ptr->getReprojectedPointcloud();
      pcl::PointCloud<pcl::PointXYZ> reprojected_pointcloud_msg;
      reprojected_pointcloud_msg.header.frame_id =
          pointcloud_msg->header.frame_id;
      reprojected_pointcloud_msg.header.stamp =
          pointcloud_msg->header.stamp.toNSec() / 1000ull;
      for (const voxblox::Point& point : reprojected_pointcloud) {
        pcl::PointXYZ reprojected_point = {point.x(), point.y(), point.z()};
        reprojected_pointcloud_msg.push_back(reprojected_point);
      }
      reprojected_pointcloud_pub_.publish(reprojected_pointcloud_msg);
    }
  }

  // Deintegrate the pointcloud that's leaving the sliding window
  if (pointcloud_deintegration_queue_length_ > 0) {
    ros::WallTime start_deintegration = ros::WallTime::now();
    servicePointcloudDeintegrationQueue();
    ros::WallTime end_deintegration = ros::WallTime::now();
    if (verbose_) {
      ROS_INFO("Finished deintegrating in %f seconds.",
               (end_deintegration - start_deintegration).toSec());
    }
  }

  timing::Timer block_remove_timer("remove_distant_blocks");
  tsdf_map_->getTsdfLayerPtr()->removeDistantBlocks(
      T_G_C.getPosition(), max_block_distance_from_body_);
  mesh_layer_->clearDistantMesh(T_G_C.getPosition(),
                                max_block_distance_from_body_);
  block_remove_timer.Stop();

  // Callback for inheriting classes.
  newPoseCallback(T_G_C);
}

// Checks if we can get the next message from queue.
bool TsdfServer::getNextPointcloudFromQueue(
    std::queue<sensor_msgs::PointCloud2::Ptr>* queue,
    sensor_msgs::PointCloud2::Ptr* pointcloud_msg, Transformation* T_G_C) {
  const size_t kMaxQueueSize = 10;
  if (queue->empty()) {
    return false;
  }
  *pointcloud_msg = queue->front();
  if (transformer_.lookupTransform((*pointcloud_msg)->header.frame_id,
                                   world_frame_,
                                   (*pointcloud_msg)->header.stamp, T_G_C)) {
    queue->pop();
    return true;
  } else {
    if (queue->size() >= kMaxQueueSize) {
      ROS_ERROR_THROTTLE(60,
                         "Input pointcloud queue getting too long! Dropping "
                         "some pointclouds. Either unable to look up transform "
                         "timestamps or the processing is taking too long.");
      while (queue->size() >= kMaxQueueSize) {
        queue->pop();
      }
    }
  }
  return false;
}

void TsdfServer::insertPointcloud(
    const sensor_msgs::PointCloud2::Ptr& pointcloud_msg_in) {
  if (!map_running_) return;

  if (pointcloud_frame_.size())
    pointcloud_msg_in->header.frame_id = pointcloud_frame_;

  if (pointcloud_msg_in->header.stamp - last_msg_time_ptcloud_ >
      min_time_between_msgs_) {
    last_msg_time_ptcloud_ = pointcloud_msg_in->header.stamp;

    // So we have to process the queue anyway... Push this back.
    pointcloud_queue_.push(pointcloud_msg_in);
  }

  Transformation T_G_C;
  sensor_msgs::PointCloud2::Ptr pointcloud_msg;
  bool processed_any = false;
  while (
      getNextPointcloudFromQueue(&pointcloud_queue_, &pointcloud_msg, &T_G_C)) {
    constexpr bool is_freespace_pointcloud = false;
    processPointCloudMessageAndInsert(pointcloud_msg, T_G_C,
                                      is_freespace_pointcloud);
    processed_any = true;
  }

  if (!processed_any) {
    return;
  }

  if (publish_pointclouds_on_update_) {
    publishPointclouds();
  }

  if (verbose_) {
    ROS_INFO_STREAM("Timings: " << std::endl << timing::Timing::Print());
    ROS_INFO_STREAM(
        "Layer memory: " << tsdf_map_->getTsdfLayer().getMemorySize());
  }
}

void TsdfServer::insertFreespacePointcloud(
    const sensor_msgs::PointCloud2::Ptr& pointcloud_msg_in) {
  if (pointcloud_msg_in->header.stamp - last_msg_time_freespace_ptcloud_ >
      min_time_between_msgs_) {
    last_msg_time_freespace_ptcloud_ = pointcloud_msg_in->header.stamp;
    // So we have to process the queue anyway... Push this back.
    freespace_pointcloud_queue_.push(pointcloud_msg_in);
  }

  Transformation T_G_C;
  sensor_msgs::PointCloud2::Ptr pointcloud_msg;
  while (getNextPointcloudFromQueue(&freespace_pointcloud_queue_,
                                    &pointcloud_msg, &T_G_C)) {
    constexpr bool is_freespace_pointcloud = true;
    processPointCloudMessageAndInsert(pointcloud_msg, T_G_C,
                                      is_freespace_pointcloud);
  }
}

void TsdfServer::integratePointcloud(
    const ros::Time& timestamp, const Transformation& T_G_C,
    std::shared_ptr<const Pointcloud> ptcloud_C,
    std::shared_ptr<const Colors> colors, const bool is_freespace_pointcloud) {
  CHECK_EQ(ptcloud_C->size(), colors->size());
  tsdf_integrator_->integratePointCloudWithObs(
      timestamp.toSec(), T_G_C, *ptcloud_C, *colors, is_freespace_pointcloud);

  if (pointcloud_deintegration_queue_length_ > 0) {
    pointcloud_deintegration_queue_.emplace_back(PointcloudDeintegrationPacket{
        timestamp, T_G_C, ptcloud_C, colors, is_freespace_pointcloud});
  } else if (submap_interval_ > 0.0) {
    pointcloud_deintegration_queue_.emplace_back(PointcloudDeintegrationPacket{
        timestamp, T_G_C, std::make_shared<Pointcloud>(Pointcloud()),
        std::make_shared<Colors>(Colors()), is_freespace_pointcloud});
  }
}

void TsdfServer::integratePointcloud(const Transformation& T_G_C,
                                     const Pointcloud& ptcloud_C,
                                     const Colors& colors,
                                     const bool is_freespace_pointcloud) {
  CHECK_EQ(ptcloud_C.size(), colors.size());
  tsdf_integrator_->integratePointCloud(T_G_C, ptcloud_C, colors,
                                        is_freespace_pointcloud);
}

void TsdfServer::servicePointcloudDeintegrationQueue() {
  while (pointcloud_deintegration_queue_length_ <
         pointcloud_deintegration_queue_.size()) {
    const PointcloudDeintegrationPacket& oldest_pointcloud_packet =
        pointcloud_deintegration_queue_.front();
    if (verbose_) {
      ROS_INFO("Deintegrating a pointcloud with %lu points.",
               oldest_pointcloud_packet.ptcloud_C->size());
    }
    tsdf_integrator_->integratePointCloud(
        oldest_pointcloud_packet.T_G_C, *oldest_pointcloud_packet.ptcloud_C,
        *oldest_pointcloud_packet.colors,
        oldest_pointcloud_packet.is_freespace_pointcloud,
        /* deintegrate */ true);
    pointcloud_deintegration_queue_.pop_front();
    map_needs_pruning_ = true;
  }
}

void TsdfServer::pruneMap() {
  timing::Timer prune_map_timer("prune_fully_deintegrated_blocks");
  size_t num_pruned_blocks = 0u;
  BlockIndexList updated_blocks_;
  tsdf_map_->getTsdfLayerPtr()->getAllUpdatedBlocks(Update::kMap,
                                                    &updated_blocks_);
  for (const BlockIndex& updated_block_index : updated_blocks_) {
    const Block<TsdfVoxel>& updated_block =
        tsdf_map_->getTsdfLayerPtr()->getBlockByIndex(updated_block_index);
    bool block_contains_observed_voxels = false;
    for (size_t linear_index = 0u; linear_index < num_voxels_per_block_;
         ++linear_index) {
      const voxblox::TsdfVoxel& voxel =
          updated_block.getVoxelByLinearIndex(linear_index);
      if (kFloatEpsilon < voxel.weight) {
        block_contains_observed_voxels = true;
        break;
      }
    }
    if (!block_contains_observed_voxels) {
      ++num_pruned_blocks;
      tsdf_map_->getTsdfLayerPtr()->removeBlock(updated_block_index);
      if (mesh_layer_->hasMeshWithIndex(updated_block_index)) {
        Mesh::Ptr mesh_ptr =
            mesh_layer_->getMeshPtrByIndex(updated_block_index);
        mesh_ptr->clear();
        mesh_ptr->updated = true;
      }
    }
  }
  prune_map_timer.Stop();

  map_needs_pruning_ = false;
  ROS_INFO_STREAM_COND(verbose_,
                       "Pruned " << num_pruned_blocks << " TSDF blocks");
}

void TsdfServer::publishAllUpdatedTsdfVoxels() {
  if (map_needs_pruning_) {
    pruneMap();
  }

  // Create a pointcloud with distance = intensity.
  pcl::PointCloud<pcl::PointXYZI> pointcloud;

  createDistancePointcloudFromTsdfLayer(tsdf_map_->getTsdfLayer(), &pointcloud);

  pointcloud.header.frame_id = world_frame_;
  tsdf_pointcloud_pub_.publish(pointcloud);
}

void TsdfServer::publishTsdfSurfacePoints() {
  if (map_needs_pruning_) {
    pruneMap();
  }

  // Create a pointcloud with distance = intensity.
  pcl::PointCloud<pcl::PointXYZRGB> pointcloud;
  const float surface_distance_thresh =
      tsdf_map_->getTsdfLayer().voxel_size() * 0.75;
  createSurfacePointcloudFromTsdfLayer(tsdf_map_->getTsdfLayer(),
                                       surface_distance_thresh, &pointcloud);

  pointcloud.header.frame_id = world_frame_;
  surface_pointcloud_pub_.publish(pointcloud);
}

void TsdfServer::publishTsdfOccupiedNodes() {
  if (map_needs_pruning_) {
    pruneMap();
  }

  // Create a pointcloud with distance = intensity.
  visualization_msgs::MarkerArray marker_array;
  createOccupancyBlocksFromTsdfLayer(tsdf_map_->getTsdfLayer(), world_frame_,
                                     &marker_array);
  occupancy_marker_pub_.publish(marker_array);
}

void TsdfServer::publishSlices() {
  if (map_needs_pruning_) {
    pruneMap();
  }

  pcl::PointCloud<pcl::PointXYZI> pointcloud;

  constexpr int kZAxisIndex = 2;
  createDistancePointcloudFromTsdfLayerSlice(
      tsdf_map_->getTsdfLayer(), kZAxisIndex, slice_level_, &pointcloud);

  pointcloud.header.frame_id = world_frame_;
  tsdf_slice_pub_.publish(pointcloud);
}

void TsdfServer::publishMap(bool reset_remote_map) {
  if (map_needs_pruning_) {
    pruneMap();
  }

  if (publish_mesh_with_history_) {
    // transformLayerToSubmapFrame();
    publishMeshWithHistory();
  }

  if (!publish_tsdf_map_) {
    return;
  }
  int subscribers = this->tsdf_map_pub_.getNumSubscribers();
  if (subscribers > 0) {
    if (num_subscribers_tsdf_map_ < subscribers) {
      // Always reset the remote map and send all when a new subscriber
      // subscribes. A bit of overhead for other subscribers, but better than
      // inconsistent map states.
      reset_remote_map = true;
    }
    const bool only_updated = !reset_remote_map;
    timing::Timer publish_map_timer("map/publish_tsdf");
    voxblox_msgs::Layer layer_msg;
    serializeLayerAsMsg<TsdfVoxel>(this->tsdf_map_->getTsdfLayer(),
                                   only_updated, &layer_msg);
    if (reset_remote_map) {
      layer_msg.action = static_cast<uint8_t>(MapDerializationAction::kReset);
    }
    if (publish_map_with_trajectory_) {
      voxblox_msgs::LayerWithTrajectory layer_with_trajectory_msg;
      layer_with_trajectory_msg.layer = layer_msg;
      for (const PointcloudDeintegrationPacket& pointcloud_queue_packet :
           pointcloud_deintegration_queue_) {
        geometry_msgs::PoseStamped pose_msg;
        pose_msg.header.frame_id = world_frame_;
        pose_msg.header.stamp = pointcloud_queue_packet.timestamp;
        tf::poseKindrToMsg(pointcloud_queue_packet.T_G_C.cast<double>(),
                           &pose_msg.pose);
        layer_with_trajectory_msg.trajectory.poses.emplace_back(pose_msg);
      }
      this->tsdf_map_pub_.publish(layer_with_trajectory_msg);
    } else {
      this->tsdf_map_pub_.publish(layer_msg);
    }
    publish_map_timer.Stop();
  }
  num_subscribers_tsdf_map_ = subscribers;
}

void TsdfServer::publishPointclouds() {
  if (map_needs_pruning_) {
    pruneMap();
  }

  // Combined function to publish all possible pointcloud messages -- surface
  // pointclouds, updated points, and occupied points.
  publishAllUpdatedTsdfVoxels();
  publishTsdfSurfacePoints();
  publishTsdfOccupiedNodes();
  if (publish_slices_) {
    publishSlices();
  }
}

void TsdfServer::updateMesh() {
  if (verbose_) {
    ROS_INFO("Updating mesh.");
  }

  timing::Timer generate_mesh_timer("mesh/update");
  constexpr bool only_mesh_updated_blocks = true;
  constexpr bool clear_updated_flag = true;
  mesh_integrator_->generateMesh(only_mesh_updated_blocks, clear_updated_flag);
  generate_mesh_timer.Stop();

  timing::Timer publish_mesh_timer("mesh/publish");

  voxblox_msgs::Mesh mesh_msg;
  generateVoxbloxMeshMsg(mesh_layer_, color_mode_, &mesh_msg);
  mesh_msg.header.frame_id = world_frame_;
  mesh_pub_.publish(mesh_msg);

  if (cache_mesh_) {
    cached_mesh_msg_ = mesh_msg;
  }

  publish_mesh_timer.Stop();

  if (publish_pointclouds_ && !publish_pointclouds_on_update_) {
    publishPointclouds();
  }
}

bool TsdfServer::generateMesh() {
  timing::Timer generate_mesh_timer("mesh/generate");
  const bool clear_mesh = true;
  if (clear_mesh) {
    constexpr bool only_mesh_updated_blocks = false;
    constexpr bool clear_updated_flag = true;
    mesh_integrator_->generateMesh(only_mesh_updated_blocks,
                                   clear_updated_flag);
  } else {
    constexpr bool only_mesh_updated_blocks = true;
    constexpr bool clear_updated_flag = true;
    mesh_integrator_->generateMesh(only_mesh_updated_blocks,
                                   clear_updated_flag);
  }
  generate_mesh_timer.Stop();

  timing::Timer publish_mesh_timer("mesh/publish");
  voxblox_msgs::Mesh mesh_msg;
  generateVoxbloxMeshMsg(mesh_layer_, color_mode_, &mesh_msg);
  mesh_msg.header.frame_id = world_frame_;
  mesh_pub_.publish(mesh_msg);

  publish_mesh_timer.Stop();

  if (!mesh_filename_.empty()) {
    timing::Timer output_mesh_timer("mesh/output");
    const bool success = outputMeshLayerAsPly(mesh_filename_, *mesh_layer_);
    output_mesh_timer.Stop();
    if (success) {
      ROS_INFO("Output file as PLY: %s", mesh_filename_.c_str());
    } else {
      ROS_INFO("Failed to output mesh as PLY: %s", mesh_filename_.c_str());
    }
  }

  ROS_INFO_STREAM("Mesh Timings: " << std::endl << timing::Timing::Print());
  return true;
}

bool TsdfServer::saveMap(const std::string& file_path) {
  // Inheriting classes should add saving other layers to this function.
  return io::SaveLayer(tsdf_map_->getTsdfLayer(), file_path);
}

bool TsdfServer::loadMap(const std::string& file_path) {
  // Inheriting classes should add other layers to load, as this will only
  // load
  // the TSDF layer.
  constexpr bool kMulitpleLayerSupport = true;
  bool success = io::LoadBlocksFromFile(
      file_path, Layer<TsdfVoxel>::BlockMergingStrategy::kReplace,
      kMulitpleLayerSupport, tsdf_map_->getTsdfLayerPtr());
  if (success) {
    LOG(INFO) << "Successfully loaded TSDF layer.";
  }
  return success;
}

bool TsdfServer::clearMapCallback(std_srvs::Empty::Request& /*request*/,
                                  std_srvs::Empty::Response&
                                  /*response*/) {  // NOLINT
  clear();
  return true;
}

bool TsdfServer::generateMeshCallback(std_srvs::Empty::Request& /*request*/,
                                      std_srvs::Empty::Response&
                                      /*response*/) {  // NOLINT
  return generateMesh();
}

bool TsdfServer::saveMapCallback(voxblox_msgs::FilePath::Request& request,
                                 voxblox_msgs::FilePath::Response&
                                 /*response*/) {  // NOLINT
  return saveMap(request.file_path);
}

bool TsdfServer::loadMapCallback(voxblox_msgs::FilePath::Request& request,
                                 voxblox_msgs::FilePath::Response&
                                 /*response*/) {  // NOLINT
  bool success = loadMap(request.file_path);
  return success;
}

bool TsdfServer::publishPointcloudsCallback(
    std_srvs::Empty::Request& /*request*/, std_srvs::Empty::Response&
    /*response*/) {  // NOLINT
  publishPointclouds();
  return true;
}

bool TsdfServer::publishTsdfMapCallback(std_srvs::Empty::Request& /*request*/,
                                        std_srvs::Empty::Response&
                                        /*response*/) {  // NOLINT
  publishMap();
  return true;
}

void TsdfServer::updateMeshEvent(const ros::TimerEvent& /*event*/) {
  updateMesh();
}

void TsdfServer::publishMapEvent(const ros::TimerEvent& /*event*/) {
  publishMap();
}

void TsdfServer::clear() {
  tsdf_map_->getTsdfLayerPtr()->removeAllBlocks();
  mesh_layer_->clear();

  // Publish a message to reset the map to all subscribers.
  if (publish_tsdf_map_) {
    constexpr bool kResetRemoteMap = true;
    publishMap(kResetRemoteMap);
  }
}

void TsdfServer::tsdfMapCallback(const voxblox_msgs::Layer& layer_msg) {
  timing::Timer receive_map_timer("map/receive_tsdf");

  bool success =
      deserializeMsgToLayer<TsdfVoxel>(layer_msg, tsdf_map_->getTsdfLayerPtr());

  if (!success) {
    ROS_ERROR_THROTTLE(10, "Got an invalid TSDF map message!");
  } else {
    ROS_INFO_ONCE("Got an TSDF map from ROS topic!");
    if (publish_pointclouds_on_update_) {
      publishPointclouds();
    }
  }
}

}  // namespace voxblox
