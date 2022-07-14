#ifndef VOXGRAPH_FRONTEND_SUBMAP_COLLECTION_VOXGRAPH_SUBMAP_COLLECTION_H_
#define VOXGRAPH_FRONTEND_SUBMAP_COLLECTION_VOXGRAPH_SUBMAP_COLLECTION_H_

#include <memory>
#include <utility>
#include <vector>

#include <cblox/core/common.h>
#include <cblox/core/submap_collection.h>
#include <geometry_msgs/PoseStamped.h>
#include <minkindr_conversions/kindr_msg.h>
#include <ros/ros.h>
#include <voxblox/core/common.h>

#include "voxgraph/common.h"
#include "voxgraph/frontend/submap_collection/submap_timeline.h"
#include "voxgraph/frontend/submap_collection/voxgraph_submap.h"
namespace voxgraph {
class VoxgraphSubmapCollection
    : public cblox::SubmapCollection<VoxgraphSubmap> {
 public:
  typedef std::shared_ptr<VoxgraphSubmapCollection> Ptr;
  typedef std::shared_ptr<const VoxgraphSubmapCollection> ConstPtr;
  typedef std::vector<geometry_msgs::PoseStamped> PoseStampedVector;

  explicit VoxgraphSubmapCollection(VoxgraphSubmap::Config submap_config,
                                    bool verbose = false)
      : SubmapCollection(submap_config),
        submap_creation_interval_(20),
        verbose_(verbose) {}

  void setSubmapCreationInterval(ros::Duration submap_creation_interval) {
    submap_creation_interval_ = std::move(submap_creation_interval);
  }

  bool shouldCreateNewSubmap(const ros::Time& current_time);

  // Voxgraph compatible submap creation methods
  // NOTE: These methods properly add the submaps to the timeline
  void createNewSubmap(const Transformation& T_odom_base,
                       const ros::Time& submap_start_time);
  void addSubmap(const VoxgraphSubmap& submap) override;
  void addSubmap(VoxgraphSubmap&& submap) override;
  void addSubmap(const typename VoxgraphSubmap::Ptr submap) override;
  void addSubmapToTimeline(const VoxgraphSubmap& submap);

  // Warn the user to avoid using the submap creation methods inherited from
  // cblox that provide insufficient time information
  void createNewSubmap(const Transformation& T_O_S,
                       const SubmapID submap_id) override;
  SubmapID createNewSubmap(const Transformation& T_O_S) override;

  SubmapID getPreviousSubmapId() const {
    return submap_timeline_.getPreviousSubmapId();
  }
  SubmapID getFirstSubmapId() const {
    return submap_timeline_.getFirstSubmapId();
  }
  SubmapID getLastSubmapId() const {
    return submap_timeline_.getLastSubmapId();
  }

  bool lookupActiveSubmapByTime(const ros::Time& timestamp,
                                SubmapID* submap_id);

  PoseStampedVector getPoseHistory() const;

  // Create a gravity aligned poses, as used for the submap origins
  // NOTE: The submap origin poses must have zero pitch and roll since
  //       the pose graph optimization only operates in 4D (x, y, z and yaw).
  static Transformation gravityAlignPose(const Transformation& input_pose);

  auto const getTimeLine() const {
    return std::make_pair(getSubmap(getFirstSubmapId()).getStartTime(),
                          getSubmap(getLastSubmapId()).getEndTime());
  }

  bool lookUpSubmapByTime(ros::Time time, VoxgraphSubmap::Ptr* submap,
                          SubmapID* csid, Transformation* T_submap_t) {
    if (lookupActiveSubmapByTime(time, csid)) {
      *submap = getSubmapPtr(*csid);
      if ((*submap)->lookupPoseByTime(time, T_submap_t)) {
        return true;
      } else {
        LOG(WARNING) << "Requested time " << time
                     << " has no corresponding robot pose!";
        return false;
      }
    } else {
      LOG(WARNING) << "No active submap containing requested time ";
      return false;
    }
  }

 private:
  bool verbose_;

  // Length of the time interval between the creation of subsequent submaps
  ros::Duration submap_creation_interval_;  // In seconds

  // Timeline to enable lookups of submaps by time
  SubmapTimeline submap_timeline_;
};
}  // namespace voxgraph

#endif  // VOXGRAPH_FRONTEND_SUBMAP_COLLECTION_VOXGRAPH_SUBMAP_COLLECTION_H_
