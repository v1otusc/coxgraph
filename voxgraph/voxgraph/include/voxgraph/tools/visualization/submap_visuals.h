#ifndef VOXGRAPH_TOOLS_VISUALIZATION_SUBMAP_VISUALS_H_
#define VOXGRAPH_TOOLS_VISUALIZATION_SUBMAP_VISUALS_H_

#include <memory>
#include <string>

#include <cblox/core/submap_collection.h>
#include <cblox/mesh/submap_mesher.h>
#include <minkindr_conversions/kindr_msg.h>
#include <minkindr_conversions/kindr_tf.h>
#include <nav_msgs/Path.h>
#include <ros/ros.h>
#include <voxblox_msgs/MultiMesh.h>
#include <voxblox_ros/mesh_vis.h>

#include "voxgraph/frontend/submap_collection/voxgraph_submap_collection.h"

namespace voxgraph {
class SubmapVisuals {
 public:
  explicit SubmapVisuals(VoxgraphSubmap::Config submap_config,
                         voxblox::MeshIntegratorConfig mesh_config);

  // Copy constructor, reset submap mesher ptr to avoid copy unique ptr
  SubmapVisuals(const SubmapVisuals& rhs);

  // TODO(victorr): Use a Config with fromRosParams(...) method that is passed
  //                to the constructor method instead
  void setMeshOpacity(float mesh_opacity) { mesh_opacity_ = mesh_opacity; }
  void setSubmapMeshColorMode(voxblox::ColorMode color_mode) {
    submap_mesh_color_mode_ = color_mode;
  }
  void setCombinedMeshColorMode(voxblox::ColorMode color_mode) {
    combined_mesh_color_mode_ = color_mode;
  }

  void generateSubmapMesh(VoxgraphSubmap::ConstPtr submap_ptr,
                          const voxblox::Color& submap_color,
                          cblox::MeshLayer* mesh_layer_ptr) const {
    voxblox::MeshIntegrator<voxblox::TsdfVoxel> reference_mesh_integrator(
        mesh_config_, submap_ptr->getTsdfMap().getTsdfLayer(), mesh_layer_ptr);
    reference_mesh_integrator.generateMesh(false, false);
    if (submap_color.r != 0 || submap_color.g != 0 || submap_color.b != 0 ||
        submap_color.a != 0)
      separated_submap_mesher_->colorMeshLayer(submap_color, mesh_layer_ptr);
  }

  void generateSubmapMeshMsg(const voxblox::MeshLayer::Ptr& mesh_layer_ptr,
                             voxblox_msgs::Mesh* mesh_msg) const {
    CHECK(mesh_msg != nullptr);
    voxblox::generateVoxbloxMeshMsg(mesh_layer_ptr, submap_mesh_color_mode_,
                                    mesh_msg);
  }

  void publishMesh(const voxblox::MeshLayer::Ptr& mesh_layer_ptr,
                   const std::string& frame_id, const ros::Publisher& publisher,
                   const voxblox::ColorMode& color_mode) const;

  void publishMultiMesh(const voxblox::MeshLayer::Ptr& mesh_layer_ptr,
                        const std::string& frame_id,
                        const ros::Publisher& publisher,
                        const voxblox::ColorMode& color_mode,
                        const SubmapID mesh_id) const;

  void publishMesh(
      const cblox::SubmapCollection<VoxgraphSubmap>& submap_collection,
      const cblox::SubmapID& submap_id, const voxblox::Color& submap_color,
      const std::string& frame_id, const ros::Publisher& publisher) const;

  void publishMesh(
      const cblox::SubmapCollection<VoxgraphSubmap>& submap_collection,
      const SubmapID& submap_id, const std::string& frame_id,
      const ros::Publisher& publisher, bool use_submap_color_mode = false);

  void publishSeparatedMesh(
      const cblox::SubmapCollection<VoxgraphSubmap>& submap_collection,
      const std::string& odom_frame, const ros::Publisher& publisher);

  void publishCombinedMesh(
      const cblox::SubmapCollection<VoxgraphSubmap>& submap_collection,
      const std::string& odom_frame, const ros::Publisher& publisher);

  void saveSeparatedMesh(
      const std::string& filepath,
      const cblox::SubmapCollection<VoxgraphSubmap>& submap_collection);

  void saveCombinedMesh(
      const std::string& filepath,
      const cblox::SubmapCollection<VoxgraphSubmap>& submap_collection);

  void getCombinedMesh(
      cblox::MeshLayer* mesh_layer_ptr,
      const cblox::SubmapCollection<VoxgraphSubmap>& submap_collection) {
    CHECK(mesh_layer_ptr != nullptr);
    combined_submap_mesher_->generateCombinedMesh(submap_collection,
                                                  mesh_layer_ptr);
  }

  void saveAndPubCombinedMesh(
      const cblox::SubmapCollection<VoxgraphSubmap>& submap_collection,
      const std::string& mission_frame, const ros::Publisher& publisher,
      const std::string& filepath);

  void publishBox(const BoxCornerMatrix& box_corner_matrix,
                  const voxblox::Color& box_color, const std::string& frame_id,
                  const std::string& name_space,
                  const ros::Publisher& publisher) const;

  void publishPoseHistory(const VoxgraphSubmapCollection& submap_collection,
                          const std::string& odom_frame,
                          const ros::Publisher& publisher) const;

 private:
  VoxgraphSubmap::Config submap_config_;
  voxblox::MeshIntegratorConfig mesh_config_;
  std::unique_ptr<cblox::SubmapMesher> separated_submap_mesher_;
  std::unique_ptr<cblox::SubmapMesher> combined_submap_mesher_;

  float mesh_opacity_;
  voxblox::ColorMode combined_mesh_color_mode_;
  voxblox::ColorMode submap_mesh_color_mode_;
  const voxblox::ExponentialOffsetIdColorMap submap_id_color_map_;
};
}  // namespace voxgraph

#endif  // VOXGRAPH_TOOLS_VISUALIZATION_SUBMAP_VISUALS_H_
