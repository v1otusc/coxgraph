#ifndef VOXGRAPH_FRONTEND_SUBMAP_COLLECTION_VOXGRAPH_SUBMAP_H_
#define VOXGRAPH_FRONTEND_SUBMAP_COLLECTION_VOXGRAPH_SUBMAP_H_

#include <map>
#include <memory>
#include <vector>

#include <cblox/core/tsdf_esdf_submap.h>
#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>

#include "voxgraph/frontend/submap_collection/bounding_box.h"
#include "voxgraph/frontend/submap_collection/registration_point.h"
#include "voxgraph/frontend/submap_collection/weighted_sampler.h"

namespace voxgraph {
class VoxgraphSubmap : public cblox::TsdfEsdfSubmap {
 public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

  typedef std::shared_ptr<VoxgraphSubmap> Ptr;
  typedef std::shared_ptr<const VoxgraphSubmap> ConstPtr;
  // TODO(victorr): Use Eigen aligned map
  typedef std::map<ros::Time, voxblox::Transformation> PoseHistoryMap;

  struct Config : cblox::TsdfEsdfSubmap::Config {
    struct RegistrationFilter {
      double min_voxel_weight = 1;
      double max_voxel_distance = 0.3;
      bool use_esdf_distance = true;
    } registration_filter;
  };

  // Standard constructor
  VoxgraphSubmap(const voxblox::Transformation& T_O_S,
                 const cblox::SubmapID& submap_id, const Config& config);
  // Copy constructor
  // NOTE: Copying the TSDF and ESDF layers is delegated to the
  //       TsdfEsdfSubmap copy constructor
  VoxgraphSubmap(const VoxgraphSubmap& rhs) = default;

  // Copy submap with a new id
  VoxgraphSubmap(const cblox::SubmapID& submap_id, const VoxgraphSubmap& rhs);

  // Move constructor
  // NOTE: Moving the TSDF and ESDF layers is delegated to the
  //       TsdfEsdfSubmap move constructor
  VoxgraphSubmap(VoxgraphSubmap&& rhs) = default;
  // Construct based on a TsdfLayer copy
  VoxgraphSubmap(const voxblox::Transformation& T_O_S,
                 const cblox::SubmapID& submap_id,
                 const voxblox::Layer<voxblox::TsdfVoxel>& tsdf_layer);

  // Setter method for the registration filter config
  // NOTE: This method is mainly useful for copy or proto constructed submaps
  void setRegistrationFilterConfig(
      const Config::RegistrationFilter& registration_filter_config);

  const ros::Time getStartTime() const;
  const ros::Time getEndTime() const;

  void addPoseToHistory(const ros::Time& timestamp,
                        const voxblox::Transformation& T_submap_base);
  const PoseHistoryMap& getPoseHistory() const { return pose_history_; }
  bool lookupPoseByTime(const ros::Time& timestamp,
                        voxblox::Transformation* T_submap_robot) const;

  // Indicate that the submap is finished and generate all cached members
  // NOTE: These cached members are mainly used in the registration cost funcs
  void finishSubmap() override;

  void transformSubmap(const voxblox::Transformation& T_new_old,
                       bool transform_layer = true);

  // The type of registration points supported by this submap
  enum class RegistrationPointType { kIsosurfacePoints = 0, kVoxels };
  // Getter to get the registration points of a certain type
  const WeightedSampler<RegistrationPoint>& getRegistrationPoints(
      RegistrationPointType registration_point_type) const;

  // Overlap and Bounding Box related methods
  bool overlapsWith(const VoxgraphSubmap& other_submap) const;
  const BoundingBox getSubmapFrameSurfaceObb() const;
  const BoundingBox getSubmapFrameSubmapObb() const;
  const BoundingBox getOdomFrameSurfaceAabb() const;
  const BoundingBox getOdomFrameSubmapAabb() const;
  const BoxCornerMatrix getOdomFrameSurfaceObbCorners() const;
  const BoxCornerMatrix getOdomFrameSubmapObbCorners() const;
  const BoxCornerMatrix getOdomFrameSurfaceAabbCorners() const;
  const BoxCornerMatrix getOdomFrameSubmapAabbCorners() const;

  // Load a submap from stream.
  // Note: Returns a nullptr if load is unsuccessful.
  static VoxgraphSubmap::Ptr LoadFromStream(const Config& config,
                                            std::fstream* proto_file_ptr,
                                            uint64_t* tmp_byte_offset_ptr);

  sensor_msgs::PointCloud2::Ptr mesh_pointcloud_;

 private:
  typedef Eigen::Matrix<voxblox::FloatingPoint, 4, 8> HomogBoxCornerMatrix;
  using TsdfVoxel = voxblox::TsdfVoxel;
  using EsdfVoxel = voxblox::EsdfVoxel;
  using TsdfLayer = voxblox::Layer<TsdfVoxel>;
  using EsdfLayer = voxblox::Layer<EsdfVoxel>;
  using TsdfBlock = voxblox::Block<TsdfVoxel>;
  using EsdfBlock = voxblox::Block<EsdfVoxel>;
  using Interpolator = voxblox::Interpolator<voxblox::TsdfVoxel>;

  Config config_;

  // Track whether this submap has been declared finished
  // and all cached values have been generated
  bool finished_ = false;

  // Oriented Bounding Boxes in submap frame
  mutable BoundingBox surface_obb_;  // around the isosurface
  mutable BoundingBox map_obb_;      // around the entire submap

  // Object containing all voxels that fall within the truncation distance
  // and have a sufficiently high weight
  WeightedSampler<RegistrationPoint> relevant_voxels_;
  void findRelevantVoxelIndices();

  // Object containing all isosurface vertices stored as [x, y, z, weight]
  WeightedSampler<RegistrationPoint> isosurface_vertices_;
  voxblox::IndexSet isosurface_blocks_;
  void findIsosurfaceVertices();

  // History of how the robot moved through the submap
  PoseHistoryMap pose_history_;
};
}  // namespace voxgraph

#endif  // VOXGRAPH_FRONTEND_SUBMAP_COLLECTION_VOXGRAPH_SUBMAP_H_
