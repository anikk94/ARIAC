/*
 * Copyright (C) 2012-2016 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <math.h>
#include <string>

#include <gazebo/common/Console.hh>
#include <ignition/math/Pose3.hh>
#include <ignition/math/Vector3.hh>
#include <ignition/math/Quaternion.hh>

#include "nist_gear/AriacScorer.h"

/////////////////////////////////////////////////
AriacScorer::AriacScorer()
{
}

/////////////////////////////////////////////////
AriacScorer::~AriacScorer()
{
}

/////////////////////////////////////////////////
void AriacScorer::NotifyOrderStarted(gazebo::common::Time time, const nist_gear::Order& order, int order_priority)
{
  AriacScorer::OrderInfo orderInfo;
  orderInfo.start_time = time;
  orderInfo.order = nist_gear::Order::ConstPtr(new nist_gear::Order(order));

  boost::mutex::scoped_lock mutexLock(this->mutex);

  orderInfo.priority = order_priority;
  // if (!this->orders.empty())
  // {
  //   // orders after the first are implicitly higher priority
  //   orderInfo.priority = 3;
  // }

  auto it = this->orders.find(order.order_id);
  if (it != this->orders.end())
  {
    gzerr << "[ARIAC ERROR] Order with duplicate ID '" << order.order_id << "'; overwriting\n";
  }

  this->orders[order.order_id] = orderInfo;
}

/////////////////////////////////////////////////
void AriacScorer::NotifyOrderUpdated(gazebo::common::Time time, ariac::OrderID_t old_order,
                                     const nist_gear::Order& order)
{
  AriacScorer::OrderUpdateInfo updateInfo;
  updateInfo.update_time = time;
  updateInfo.original_order_id = old_order;
  updateInfo.order = nist_gear::Order::ConstPtr(new nist_gear::Order(order));

  boost::mutex::scoped_lock mutexLock(this->mutex);
  auto it = this->orders.find(order.order_id);
  if (it != this->orders.end())
  {
    gzerr << "[ARIAC ERROR] Asked to update nonexistant order '" << order.order_id << "'; ignoring\n";
    return;
  }

  this->order_updates.push_back(updateInfo);
}

void AriacScorer::NotifyKittingShipmentReceived(gazebo::common::Time time, ariac::KittingShipmentType_t type,
                                                const nist_gear::DetectedKittingShipment& shipment)
{
  // gzdbg << "NotifyKittingShipmentReceived\n";
  // information about the shipment that was actually submitted by the participant.
  AriacScorer::KittingShipmentInfo submitted_shipment_info;
  submitted_shipment_info.submit_time = time;
  submitted_shipment_info.type = type;  // type of the shipment, e.g., order_0_shipment_0
  submitted_shipment_info.station = shipment.tray_content.kit_tray;

  submitted_shipment_info.shipment =
      nist_gear::DetectedKittingShipment::ConstPtr(new nist_gear::DetectedKittingShipment(shipment));

  boost::mutex::scoped_lock mutexLock(this->mutex);
  this->received_kitting_shipments_vec.push_back(submitted_shipment_info);
  // gzdbg << "<<<<< END NotifyKittingShipmentReceived\n";
}

void AriacScorer::NotifyAssemblyShipmentReceived(gazebo::common::Time time, ariac::AssemblyShipmentType_t type,
                                                 const nist_gear::DetectedAssemblyShipment& shipment,
                                                 std::string actual_station)
{
  // gzdbg << "NotifyAssemblyShipmentReceived\n";
  // information about the shipment that was actually submitted by the participant.
  AriacScorer::AssemblyShipmentInfo submitted_shipment_info;
  submitted_shipment_info.submit_time = time;
  submitted_shipment_info.type = type;  // type of the shipment, e.g., order_0_shipment_0
  submitted_shipment_info.station = actual_station;
  submitted_shipment_info.shipment =
      nist_gear::DetectedAssemblyShipment::ConstPtr(new nist_gear::DetectedAssemblyShipment(shipment));

  boost::mutex::scoped_lock mutexLock(this->mutex);
  this->received_assembly_shipments_vec.push_back(submitted_shipment_info);
}

/////////////////////////////////////////////////
void AriacScorer::NotifyArmArmCollision(gazebo::common::Time /*time*/)
{
  boost::mutex::scoped_lock mutexLock(this->mutex);
  this->arm_arm_collision = true;
}

/////////////////////////////////////////////////
ariac::GameScore AriacScorer::GetGameScore(int penalty)
{
  //  try {
  // using a local lock_guard to lock mtx guarantees unlocking on destruction / exception:
  boost::mutex::scoped_lock mutexLock(this->mutex);
  // }
  // catch (std::logic_error& err) {
  //   ROS_ERROR_STREAM("[exception caught]\n");
  //   ROS_ERROR_STREAM(err.what());
  // }

  // gzdbg << "Get game score" << std::endl;
  ariac::GameScore game_score;
  game_score.penalty = penalty;

  // arm/arm collision results in zero score, but keep going for logging
  game_score.was_arm_arm_collision = this->arm_arm_collision;

  // Calculate the current score based on received orders and shipments
  // For each order, how many shipments was it supposed to have?
  for (auto& opair : this->orders)  // this->orders has order info from ariac.world
  {
    auto order_id = opair.first;
    auto order_info = opair.second;

    gazebo::common::Time start_time = order_info.start_time;
    int priority = order_info.priority;
    nist_gear::Order::ConstPtr order = order_info.order;

    // If order was updated, score based on the latest version of it
    for (auto& update_info : this->order_updates)
    {
      if (update_info.original_order_id == order_id)
      {
        // gzdbg << "+++++ order updated"  << std::endl;
        order = update_info.order;
        start_time = update_info.update_time;
      }
    }

    // Create score class for orderGetShipmentScore
    ariac::OrderScore order_score;
    order_score.order_id = order_id;  // e.g., order_0

    order_score.priority = priority;  // e.g., 1
    auto oit = game_score.order_scores_map.find(order_id);
    if (oit != game_score.order_scores_map.end())
    {
      gzerr << "[ARIAC ERROR] Multiple orders of duplicate ids:" << order_score.order_id << "\n";
    }

    std::vector<std::string> claimed_shipments;

    ////////////////////////////////////
    //  Take care of kitting shipments
    ////////////////////////////////////
    if (!this->received_kitting_shipments_vec.empty())
    {
      // gzdbg << "received_kitting_shipments_vec NOT EMPTY\n";
      // instantiate shipment_score for expected shipments (from ariac.world)
      for (const auto& expected_shipment : order->kitting_shipments)
      {
        ariac::KittingShipmentScore shipment_score;

        shipment_score.kitting_shipment_type_t = expected_shipment.shipment_type;  // e.g., order_0_kitting_shipment_0
        auto it = order_score.kitting_shipment_scores.find(expected_shipment.shipment_type);
        if (it != order_score.kitting_shipment_scores.end())
        {
          gzerr << "[ARIAC ERROR] Order contained duplicate shipment types:" << expected_shipment.shipment_type << "\n";
        }
        order_score.kitting_shipment_scores[expected_shipment.shipment_type] = shipment_score;
      }

      // Find actual shipments that belong to this order
      for (const auto& desired_shipment : order->kitting_shipments)
      {
        for (const auto& received_shipment_info : this->received_kitting_shipments_vec)
        {
          // ROS_WARN_STREAM("desired_shipment.shipment_type " << desired_shipment.shipment_type);
          // ROS_WARN_STREAM("received_shipment_info.type " << received_shipment_info.type);
          if (desired_shipment.shipment_type == received_shipment_info.type)
          {
            if (received_shipment_info.submit_time < start_time)
            {
              // Maybe order was updated, this shipment was submitted too early
              continue;
            }
            // else{
            //   ROS_WARN_STREAM("ALL GOOD");
            // }
            // If the same shipment was submitted twice, only count the first one
            bool is_claimed = false;
            for (const auto& type : claimed_shipments)
            {
              if (type == desired_shipment.shipment_type)
              {
                is_claimed = true;
                break;
              }
            }
            if (is_claimed)
            {
              continue;
            }

            claimed_shipments.push_back(desired_shipment.shipment_type);

            order_score.kitting_shipment_scores[desired_shipment.shipment_type] = this->GetKittingShipmentScore(
                received_shipment_info.submit_time, desired_shipment, *(received_shipment_info.shipment));
          }
        }
      }

      // Figure out the time taken to complete an order
      if (order_score.isKittingComplete())
      {
        // The latest submitted shipment time is the order completion time
        gazebo::common::Time end = start_time;
        for (auto& sspair : order_score.kitting_shipment_scores)
        {
          if (sspair.second.submit_time > end)
          {
            end = sspair.second.submit_time;
          }
        }
        order_score.time_taken = (end - start_time).Double();
      }
    }  // end if (!received_kitting_shipments_vec.empty())

    ////////////////////////////////////
    // Take care of assembly shipments
    ////////////////////////////////////
    if (!this->received_assembly_shipments_vec.empty())
    {
      // gzerr << "Assembly" << std::endl;
      // instantiate shipment_score for expected shipments (from ariac.world)
      for (const auto& expected_assembly_shipment : order->assembly_shipments)
      {
        ariac::AssemblyShipmentScore shipment_score;

        shipment_score.assemblyShipmentType =
            expected_assembly_shipment.shipment_type;  // e.g., order_0_assembly_shipment_0
        // gzdbg << "shipment type: " << shipment_score.assemblyShipmentType << std::endl;
        auto it = order_score.assembly_shipment_scores.find(expected_assembly_shipment.shipment_type);
        if (it != order_score.assembly_shipment_scores.end())
        {
          gzerr << "[ARIAC ERROR] Order contained duplicate shipment types:" << expected_assembly_shipment.shipment_type
                << "\n";
        }
        order_score.assembly_shipment_scores[expected_assembly_shipment.shipment_type] = shipment_score;
      }

      // Find actual shipments that belong to this order
      for (const auto& desired_assembly_shipment : order->assembly_shipments)
      {
        // gzdbg << "1" << std::endl;
        for (const auto& received_shipment_info : this->received_assembly_shipments_vec)
        {
          // gzdbg << "2" << std::endl;
          if (desired_assembly_shipment.shipment_type == received_shipment_info.type)
          {
            if (received_shipment_info.submit_time < start_time)
            {
              // Maybe order was updated, this shipment was submitted too early
              continue;
            }
            // If the same shipment was submitted twice, only count the first one
            bool is_claimed = false;
            for (const auto& type : claimed_shipments)
            {
              if (type == desired_assembly_shipment.shipment_type)
              {
                is_claimed = true;
                break;
              }
            }
            if (is_claimed)
            {
              continue;
            }

            claimed_shipments.push_back(desired_assembly_shipment.shipment_type);

            order_score.assembly_shipment_scores[desired_assembly_shipment.shipment_type] =
                this->GetAssemblyShipmentScore(received_shipment_info.submit_time, desired_assembly_shipment,
                                               *(received_shipment_info.shipment), received_shipment_info.station);
          }
        }
      }
      // Figure out the time taken to complete an order
      if (order_score.isAssemblyComplete())
      {
        // gzdbg << "Assembly completed" << std::endl;
        // The latest submitted shipment time is the order completion time
        gazebo::common::Time end = start_time;
        for (auto& sspair : order_score.assembly_shipment_scores)
        {
          if (sspair.second.submit_time > end)
          {
            end = sspair.second.submit_time;
          }
        }
        order_score.time_taken = (end - start_time).Double();
      }
    }
    // gzdbg << "1- order id" << std::endl;
    game_score.order_scores_map[order_id] = order_score;
    // gzdbg << "2- order id" << std::endl;
  }
  return game_score;
}

/**
 * @brief Compute the score for a kitting shipment
 *
 * @param submit_time Time at which the shipment was submitted
 * @param expected_shipment Shipment specified on /ariac/orders
 * @param received_shipment Shipment received from competitors
 * @param station Assembly station the shipment was submitted
 * @return ariac::KittingShipmentScore
 */
ariac::KittingShipmentScore AriacScorer::GetKittingShipmentScore(
    gazebo::common::Time submit_time, const nist_gear::ExpectedKittingShipment& expected_shipment,
    const nist_gear::DetectedKittingShipment& detected_shipment)
{
  ariac::KittingShipmentScore scorer;
  // if this function is called it means the shipment was submitted
  scorer.is_kitting_shipment_submitted = true;
  scorer.submit_time = submit_time;
  std::vector<nist_gear::DetectedProduct> wrong_color_products;

  bool has_faulty_product = false;
  bool is_missing_products = false;
  bool has_unwanted_product = false;
  scorer.product_only_type_presence = 0;
  scorer.product_only_type_and_color_presence = 0;
  scorer.all_products_bonus = 0;
  scorer.product_pose = 0;
  scorer.has_correct_agv = false;
  scorer.has_correct_station = false;
  scorer.has_correct_movable_tray_type = false;
  scorer.has_correct_movable_tray_pose = true;
  scorer.has_correct_movable_tray_position = true;
  scorer.has_correct_movable_tray_orientation = true;

  // Check the correct movable tray type was used
  //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  auto detected_movable_tray_type = detected_shipment.tray_content.movable_tray.movable_tray_type;
  auto expected_movable_tray_type = expected_shipment.tray_content.movable_tray.movable_tray_type;

  if (detected_movable_tray_type == expected_movable_tray_type)
  {
    scorer.has_correct_movable_tray_type = true;
  }

  // Check the detected movable tray has the correct pose
  // max translation diff = 10 cm
  // max orientation diff = 20 deg (0.35 rad)
  //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  const double movable_tray_translation_target = 0.1;   // 10 cm
  const double movable_tray_orientation_target = 0.35;  // 0.35 rad
  auto detected_movable_tray_position = detected_shipment.tray_content.movable_tray.movable_tray_pose.position;
  auto expected_movable_tray_position = expected_shipment.tray_content.movable_tray.movable_tray_pose.position;
  auto detected_movable_tray_orientation = detected_shipment.tray_content.movable_tray.movable_tray_pose.orientation;
  auto expected_movable_tray_orientation = expected_shipment.tray_content.movable_tray.movable_tray_pose.orientation;
  // get translation distance
  ignition::math::Vector3d movable_tray_trans_diff(expected_movable_tray_position.x - detected_movable_tray_position.x,
                                                   expected_movable_tray_position.y - detected_movable_tray_position.y,
                                                   0);
  const double movable_tray_distance = movable_tray_trans_diff.Length();
  if (movable_tray_distance > movable_tray_translation_target)
  {
    scorer.has_correct_movable_tray_position = false;
  }
  // get orientation difference
  ignition::math::Quaterniond expected_orientation(
      expected_movable_tray_orientation.w, expected_movable_tray_orientation.x, expected_movable_tray_orientation.y,
      expected_movable_tray_orientation.z);

  ignition::math::Quaterniond detected_orientation(
      detected_movable_tray_orientation.w, detected_movable_tray_orientation.x, detected_movable_tray_orientation.y,
      detected_movable_tray_orientation.z);

  // If the quaternions represent the same orientation, q1 = +-q2 => q1.dot(q2) = +-1
  const double orientation_diff = detected_orientation.Dot(expected_orientation);
  const double quaternion_diff_thresh = 0.05;

  if (std::abs(orientation_diff) < (1.0 - quaternion_diff_thresh))
  {
    scorer.has_correct_movable_tray_orientation = false;
  }
  else
  {
    // Filter the yaw based on a threshold set in radians (more user-friendly).
    // Account for wrapping in angles. E.g. -pi compared with pi should "pass".
    double angle_diff = detected_orientation.Yaw() - expected_orientation.Yaw();
    if ((std::abs(angle_diff) < movable_tray_orientation_target) ||
        (std::abs(std::abs(angle_diff) - 2 * M_PI) <= movable_tray_orientation_target))
    {
      scorer.has_correct_movable_tray_orientation = true;
    }
  }
  scorer.has_correct_movable_tray_pose =
      scorer.has_correct_movable_tray_orientation && scorer.has_correct_movable_tray_position;

  // Check correct AGV was used
  //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  auto detected_agv = detected_shipment.tray_content.kit_tray;
  auto expected_agv = expected_shipment.tray_content.kit_tray;

  // gzerr << "AGV used: " << detected_agv << std::endl;
  // make sure the kit was built on the correct agv
  if ("any" == expected_shipment.tray_content.kit_tray)
  {
    scorer.has_correct_agv = true;
  }
  else if ("agv1" == expected_agv)
  {
    scorer.has_correct_agv = "agv1" == detected_agv;
  }
  else if ("agv2" == expected_agv)
  {
    scorer.has_correct_agv = "agv2" == detected_agv;
  }
  else if ("agv3" == expected_agv)
  {
    scorer.has_correct_agv = "agv3" == detected_agv;
  }
  else if ("agv4" == expected_agv)
  {
    scorer.has_correct_agv = "agv4" == detected_agv;
  }
  else
  {
    gzerr << "[ARIAC ERROR] desired shipment agv invalid:" << expected_agv << "\n";
  }

  auto detected_station = detected_shipment.assembly_station;
  auto expected_station = expected_shipment.assembly_station;
  // make sure the AGV was sent to the correct station
  if ("as1" == expected_station)
  {
    scorer.has_correct_station = "as1" == detected_station;
  }
  else if ("as2" == expected_station)
  {
    scorer.has_correct_station = "as2" == detected_station;
  }
  else if ("as3" == expected_station)
  {
    scorer.has_correct_station = "as3" == detected_station;
  }
  else if ("as4" == expected_station)
  {
    scorer.has_correct_station = "as4" == detected_station;
  }
  else
  {
    gzerr << "[ARIAC ERROR] desired shipment station invalid:" << expected_station << "\n";
  }
  // Separate faulty and non-faulty products
  std::vector<nist_gear::DetectedProduct> detected_non_faulty_products;
  for (const auto& detected_product : detected_shipment.tray_content.products)
  {
    if (detected_product.is_faulty)
      has_faulty_product = true;
    else
      detected_non_faulty_products.push_back(detected_product);
  }

  // make a copy of non faulty products
  // we will work with this vector for actual products in the trays
  std::vector<nist_gear::DetectedProduct> tmp_non_faulty_products;
  tmp_non_faulty_products = detected_non_faulty_products;
  // check product type is correct even if color is wrong
  for (size_t d = 0; d < expected_shipment.tray_content.products.size(); ++d)
  {
    auto expected_product_type_tmp = expected_shipment.tray_content.products[d].type;
    auto expected_product_type = expected_product_type_tmp;
    // ROS_WARN_STREAM("desired_product_name: " << desired_product_name);

    // keep only the product type and remove the product color
    //-- e.g., assembly_battery_blue becomes assembly_battery
    // gzdbg << "product: " << desired_product << "\n";
    expected_product_type.erase(expected_product_type_tmp.rfind('_'));
    // gzdbg << "product type: " << desired_product_type << "\n";

    for (size_t a = 0; a < tmp_non_faulty_products.size(); ++a)
    {
      auto detected_product_name = tmp_non_faulty_products[a].type;
      // gzdbg << "actual product name: " << actual_product_name << "\n";

      // In the case the actual_product_name has the following format: agv2::tray_2::assembly_battery_blue
      // auto pos = detected_product_name.rfind(':');
      // if (pos != std::string::npos) {
      //   detected_product_name.erase(0, pos + 1);
      // }
      // now, let's grab only the type of the product and discard the color
      auto detected_product_type = detected_product_name.erase(detected_product_name.rfind('_'));

      // if we have a match in term of product type
      if (expected_product_type.compare(detected_product_type) == 0)
      {
        // found_exact_product_type = true;

        // give 1pt for correct type
        scorer.product_only_type_presence++;
        // we are done with this part
        // @todo: Do not remove this part yet
        tmp_non_faulty_products.erase(tmp_non_faulty_products.begin() + a);
        break;
      }
    }
  }

  // Map of product type to indexes in desired products (first) and indexes in non faulty actual products (second)
  std::map<std::string, std::pair<std::vector<size_t>, std::vector<size_t>>> product_type_map;

  for (size_t d = 0; d < expected_shipment.tray_content.products.size(); ++d)
  {
    // aliases
    const auto& expected_product = expected_shipment.tray_content.products[d];
    auto& mapping = product_type_map[expected_product.type];
    mapping.first.push_back(d);
  }

  for (size_t a = 0; a < detected_non_faulty_products.size(); ++a)
  {
    auto& detected_product = detected_non_faulty_products[a];
    // gzdbg << "detected product: " << actual_product.type << "\n";
    // if the name contains :: then clean the name
    // auto pos = detected_product.type.rfind(':');
    // if (pos != std::string::npos) {
    //   detected_product.type.erase(0, pos + 1);
    // }

    // 0u = 0 unsigned
    if (0u == product_type_map.count(detected_product.type))
    {
      // since desired products were put into the type map first, this product must be unwanted
      has_unwanted_product = true;
      // add this unwanted product to a different list
      // we will reuse it later to check its pose
      wrong_color_products.push_back(detected_product);
      continue;
    }
    auto& mapping = product_type_map.at(detected_product.type);
    mapping.second.push_back(a);
  }

  for (const auto& type_pair : product_type_map)
  {
    const std::vector<size_t>& expected_indexes = type_pair.second.first;
    const std::vector<size_t>& detected_indexes = type_pair.second.second;
    auto product_name = type_pair.first;
    // gzdbg << "desired_indexes: " << desired_indexes.size() << "\n";
    // gzdbg << "actual_indexes: " << actual_indexes.size() << "\n";
    // gzdbg << "product_name: " << product_name << "\n";

    if (expected_indexes.size() > detected_indexes.size())
    {
      is_missing_products = true;
    }
    else if (expected_indexes.size() < detected_indexes.size())
    {
      has_unwanted_product = true;
    }

    // no point in trying to score this type if there are none delivered
    if (detected_indexes.empty())
    {
      continue;
    }

    scorer.product_only_type_and_color_presence += std::min(expected_indexes.size(), detected_indexes.size());

    double contributing_pose_score = 0;
    size_t num_indices = std::max(expected_indexes.size(), detected_indexes.size());
    // ROS_WARN_STREAM("num_indices: "<< num_indices);
    std::vector<size_t> permutation(num_indices);
    for (size_t i = 0; i < num_indices; ++i)
    {
      permutation[i] = i;
    }

    // check if poses are correct
    // iterate through all permutations of actual matched with desired to find the highest pose score
    do
    {
      double permutation_pose_score = 0;
      for (size_t d = 0; d < expected_indexes.size(); ++d)
      {
        const size_t actual_index_index = permutation[d];
        if (actual_index_index >= detected_indexes.size())
        {
          // there were fewer actual products than the order called for
          continue;
        }
        const auto& expected_product = expected_shipment.tray_content.products[expected_indexes[d]];
        const auto& detected_product = detected_non_faulty_products[detected_indexes[actual_index_index]];

        // Add points for each product in the correct pose
        const double translation_target = 0.03;  // 3 cm
        const double orientation_target = 0.1;   // 0.1 rad
        // get translation distance
        ignition::math::Vector3d posnDiff(expected_product.pose.position.x - detected_product.pose.position.x,
                                          expected_product.pose.position.y - detected_product.pose.position.y, 0);
        // ROS_WARN_STREAM("desired_product.pose: " <<
        // desired_product.pose.position.x << ", " << desired_product.pose.position.y);
        // ROS_WARN_STREAM("actual_product.pose: " <<
        // actual_product.pose.position.x << ", " << actual_product.pose.position.y);

        const double distance = posnDiff.Length();

        if (distance > translation_target)
        {
          // Skipping product because translation error is too big
          continue;
        }

        ignition::math::Quaterniond expectedOrientation(
            expected_product.pose.orientation.w, expected_product.pose.orientation.x,
            expected_product.pose.orientation.y, expected_product.pose.orientation.z);

        ignition::math::Quaterniond detectedOrientation(
            detected_product.pose.orientation.w, detected_product.pose.orientation.x,
            detected_product.pose.orientation.y, detected_product.pose.orientation.z);

        // Filter products that aren't in the appropriate orientation (loosely).
        // If the quaternions represent the same orientation, q1 = +-q2 => q1.dot(q2) = +-1
        const double orientationDiff = detectedOrientation.Dot(expectedOrientation);
        // TODO(zeid): this value can probably be derived using relationships between
        // euler angles and quaternions.
        const double quaternionDiffThresh = 0.05;
        if (std::abs(orientationDiff) < (1.0 - quaternionDiffThresh))
        {
          // Skipping product because it is not in the correct orientation (roughly)
          continue;
        }

        // Filter the yaw based on a threshold set in radians (more user-friendly).
        // Account for wrapping in angles. E.g. -pi compared with pi should "pass".
        double angleDiff = detectedOrientation.Yaw() - expectedOrientation.Yaw();
        if ((std::abs(angleDiff) < orientation_target) ||
            (std::abs(std::abs(angleDiff) - 2 * M_PI) <= orientation_target))
        {
          permutation_pose_score += 1.0;
        }
      }
      if (permutation_pose_score > contributing_pose_score)
      {
        contributing_pose_score = permutation_pose_score;
      }
    } while (std::next_permutation(permutation.begin(), permutation.end()));

    // Add the pose score contributed by the highest scoring permutation
    scorer.product_pose += contributing_pose_score;

    // let's see if unwanted products have the correct pose
    for (const auto& type_pair : product_type_map)
    {
      const std::vector<size_t>& desired_indexes = type_pair.second.first;
      for (size_t d = 0; d < desired_indexes.size(); ++d)
      {
        const auto desired_product = expected_shipment.tray_content.products[desired_indexes[d]];
        // gzdbg << "desired product: " << desired_product.type << "\n";
        for (const auto& wrong_color_product : wrong_color_products)
        {
          // gzdbg << "wrong color product: " << wrong_color_product.type << "\n";

          // check if desired and actual products are of the same type (regardless of color)
          auto desired_product_type = desired_product.type;
          desired_product_type.erase(desired_product_type.rfind('_'));

          auto actual_product_type = wrong_color_product.type;
          actual_product_type.erase(actual_product_type.rfind('_'));

          // check the pose if we are talking about the same product type
          if (actual_product_type.compare(desired_product_type) == 0)
          {
            // Add points for each product in the correct pose
            const double translation_target = 0.03;  // 3 cm
            const double orientation_target = 0.1;   // 0.1 rad
            // get translation distance
            ignition::math::Vector3d posnDiff(desired_product.pose.position.x - wrong_color_product.pose.position.x,
                                              desired_product.pose.position.y - wrong_color_product.pose.position.y, 0);

            const double distance = posnDiff.Length();

            if (distance > translation_target)
            {
              // skipping product because translation error is too big
              continue;
            }

            ignition::math::Quaterniond expectedOrientation(
                desired_product.pose.orientation.w, desired_product.pose.orientation.x,
                desired_product.pose.orientation.y, desired_product.pose.orientation.z);

            ignition::math::Quaterniond detectedOrientation(
                wrong_color_product.pose.orientation.w, wrong_color_product.pose.orientation.x,
                wrong_color_product.pose.orientation.y, wrong_color_product.pose.orientation.z);

            // Filter products that aren't in the appropriate orientation (loosely).
            // If the quaternions represent the same orientation, q1 = +-q2 => q1.dot(q2) = +-1
            const double orientationDiff = detectedOrientation.Dot(expectedOrientation);
            // TODO(zeid): this value can probably be derived using relationships between
            // euler angles and quaternions.
            const double quaternionDiffThresh = 0.05;
            if (std::abs(orientationDiff) < (1.0 - quaternionDiffThresh))
            {
              // skipping product because it is not in the correct orientation (roughly)
              continue;
            }

            // filter the yaw based on a threshold set in radians (more user-friendly).
            // account for wrapping in angles. E.g. -pi compared with pi should "pass".
            double angleDiff = detectedOrientation.Yaw() - expectedOrientation.Yaw();

            if ((std::abs(angleDiff) < orientation_target) ||
                (std::abs(std::abs(angleDiff) - 2 * M_PI) <= orientation_target))
            {
              scorer.product_pose++;
            }
          }
        }
      }
    }

    // check if pose is correct even for products with wrong colors but with the correct type
  }

  if (!is_missing_products)
  {
    scorer.is_kitting_shipment_complete = true;
  }
  if (!has_faulty_product && !has_unwanted_product && !is_missing_products)
  {
    // allProductsBonus is applied if all products have:
    // -the correct pose
    // -the correct color
    // -the correct type
    // -the product is not faulty
    // correct type is true AND correct color is true AND correct pose is true
    if (scorer.product_pose == expected_shipment.tray_content.products.size())
    {
      if (scorer.product_only_type_and_color_presence == expected_shipment.tray_content.products.size())
      {
        if (scorer.product_only_type_presence == detected_shipment.tray_content.products.size())
        {
          scorer.all_products_bonus = expected_shipment.tray_content.products.size();
        }
      }
    }
  }

  return scorer;
}

/////////////////////////////////////////////////////////////////////////
ariac::AssemblyShipmentScore AriacScorer::GetAssemblyShipmentScore(
    gazebo::common::Time submit_time, const nist_gear::AssemblyShipment& desired_shipment,
    const nist_gear::DetectedAssemblyShipment& actual_shipment, std::string station)
{
  // gzerr << "Assembly shipment score" << std::endl;
  ariac::AssemblyShipmentScore scorer;

  scorer.assemblyStation = station;   // assembly station this shipment was sent for
  scorer.submit_time = submit_time;   // time the shipment was submitted
  scorer.isShipmentSubmitted = true;  // true since we are in this function
  scorer.allProductsBonus = 0;
  scorer.isCorrectStation = false;
  scorer.numberOfProductsInShipment = actual_shipment.products.size();
  scorer.desiredNumberOfProducts = desired_shipment.products.size();

  std::string ACTUAL_PRODUCT_TYPE;

  std::map<std::string, ariac::BriefcaseProduct> mapOfBriefcaseProducts;

  // was the shipment submitted from the desired assembly station?
  if ("as1" == desired_shipment.station_id)
  {
    scorer.isCorrectStation = "as1" == station;
  }
  else if ("as2" == desired_shipment.station_id)
  {
    scorer.isCorrectStation = "as2" == station;
  }
  else if ("as3" == desired_shipment.station_id)
  {
    scorer.isCorrectStation = "as3" == station;
  }
  else if ("as4" == desired_shipment.station_id)
  {
    scorer.isCorrectStation = "as4" == station;
  }
  else
  {
    gzerr << "[ARIAC ERROR] desired shipment station invalid:" << desired_shipment.station_id << "\n";
  }

  // check for faulty products and set hasFaultyProduct=true if faulty products exist
  // store non-faulty products in a vector
  std::vector<nist_gear::DetectedProduct> detected_non_faulty_products;

  for (const auto& actual_product : actual_shipment.products)
  {
    if (actual_product.is_faulty)
    {
      scorer.hasFaultyProduct = true;
    }
    else
    {
      detected_non_faulty_products.push_back(actual_product);
    }
  }

  // Check if the shipment contains missing products
  if (detected_non_faulty_products.size() < desired_shipment.products.size()) {
    scorer.hasMissingProduct = true;
  }

   if (!scorer.hasMissingProduct)
  {
    scorer.isShipmentComplete = true;
  }

  // make a copy of non-faulty products
  std::vector<nist_gear::DetectedProduct> tmp_non_faulty_products;
  tmp_non_faulty_products = detected_non_faulty_products;

  // compare each desired product with actual product
  for (size_t d = 0; d < desired_shipment.products.size(); ++d)
  {
    auto desired_product_pose = desired_shipment.products[d].pose;
    auto desired_product = desired_shipment.products[d].type;
    auto desired_product_name = desired_product;

    // filter out the product color from the part name to get the product type
    // e.g., assembly_battery_blue becomes assembly_battery
    auto desired_product_type = desired_product.erase(desired_product.rfind('_'));
    std::string tmp = desired_product_type.substr(9);  // get from "assembly_" to the end
    auto pos = tmp.find("_");
    desired_product_type = tmp.substr(0, pos);
    // gzerr << "desired product type: " << desired_product_type << std::endl;


    std::size_t found = desired_product_name.find_last_of("_");
    auto desired_product_color = desired_product_name.substr(found + 1);
    // gzerr << "desired product color: " << desired_product_color << std::endl;

    for (size_t a = 0; a < tmp_non_faulty_products.size(); ++a)
    {
      ariac::BriefcaseProduct briefcaseProduct;
      auto actual_product = tmp_non_faulty_products[a].type;
      auto actual_product_name = actual_product;
      auto actual_product_pose = tmp_non_faulty_products[a].pose;
      // in the case the actual_product_name has the following format: agv2::tray_2::assembly_battery_blue
      // auto pos = actual_product.rfind(':');
      // if (pos != std::string::npos) {
      //   actual_product_name.erase(0, pos + 1);
      // }
      // gzerr << "actual product name: " << actual_product_name << std::endl;
      std::size_t last_underscore_occurrence = actual_product_name.find_last_of("_");
      std::string actual_product_color = actual_product_name.substr(last_underscore_occurrence + 1);
      // gzerr << "actual product color: " << actual_product_color << std::endl;  // assembly_pump_blue

      std::size_t pos = actual_product_name.find("assembly_");  // position of "assembly_" in actual_product_name
      std::string tmp = actual_product_name.substr(9);          // get from "assembly_" to the end

      pos = tmp.find("_");
      std::string actual_product_type = tmp.substr(0, pos);
      // gzerr << "actual product type: " << actual_product_type << std::endl;  // assembly_pump_blue

      briefcaseProduct.productName = actual_product_name;
      briefcaseProduct.productType = actual_product_type;

      // correct type
      if (desired_product_type.compare(actual_product_type) == 0)
      {
        briefcaseProduct.isProductCorrectType = true;
        scorer.numberOfProductsWithCorrectType++;
        // correct color
        if (desired_product_color.compare(actual_product_color) == 0)
        {
          briefcaseProduct.isProductCorrectColor = true;
          scorer.numberOfProductsWithCorrectColor++;
        }
        else {
          scorer.hasUnwantedProduct = true;
        }


        //Check the pose of the actual product with the pose of the desired product
        // Add points for each product in the correct pose
        const double translation_target = 0.02;  // 2 cm
        const double orientation_target = 0.2;   // 0.1 rad
        // get translation distance
        ignition::math::Vector3d posnDiff(desired_product_pose.position.x - actual_product_pose.position.x,
                                          desired_product_pose.position.y - actual_product_pose.position.y, 0);
        // ROS_WARN_STREAM("Position -- desired_product.pose: " <<
        // desired_product.pose.position.x << ", " << desired_product.pose.position.y);
        // ROS_WARN_STREAM("Position -- actual_product.pose: " <<
        // actual_product.pose.position.x << ", " << actual_product.pose.position.y);

        const double distance = posnDiff.Length();
        // ROS_WARN_STREAM("Position -- distance: " << distance);

        if (distance > translation_target)
        {
          // Skipping product because translation error is too big
          // ROS_WARN_STREAM("p -- SKIPPING PRODUCT ");
          continue;
        }

        ignition::math::Quaterniond orderOrientation(
            desired_product_pose.orientation.w, desired_product_pose.orientation.x, desired_product_pose.orientation.y,
            desired_product_pose.orientation.z);

        ignition::math::Quaterniond objOrientation(actual_product_pose.orientation.w, actual_product_pose.orientation.x,
                                                   actual_product_pose.orientation.y,
                                                   actual_product_pose.orientation.z);

        // Filter products that aren't in the appropriate orientation (loosely).
        // If the quaternions represent the same orientation, q1 = +-q2 => q1.dot(q2) = +-1
        const double orientationDiff = objOrientation.Dot(orderOrientation);
        // TODO(zeid): this value can probably be derived using relationships between
        // euler angles and quaternions.
        const double quaternionDiffThresh = 0.05;

        // ROS_WARN_STREAM("Orientation -- desired_product: " <<
        // desired_product.pose.positoriention.x << ", " << desired_product.pose.position.y);
        // ROS_WARN_STREAM("Orientation -- actual_product: " <<
        // actual_product.pose.position.x << ", " << actual_product.pose.position.y);

        auto diff_orientation = std::abs(orientationDiff) < (1.0 - quaternionDiffThresh);
        // ROS_WARN_STREAM("orientationDiff: " << std::abs(orientationDiff));
        // ROS_WARN_STREAM("1.0 - quaternionDiffThresh: " << 1.0 - quaternionDiffThresh);

        if (std::abs(orientationDiff) < (1.0 - quaternionDiffThresh))
        {
          // ROS_WARN_STREAM("o -- SKIPPING PRODUCT ");
          // Skipping product because it is not in the correct orientation (roughly)
          continue;
        }

        // it->second.isProductCorrectPose = true;

        scorer.numberOfProductsWithCorrectPose++;
        briefcaseProduct.isProductCorrectPose = true;

        mapOfBriefcaseProducts.insert(
            std::pair<std::string, ariac::BriefcaseProduct>(actual_product, briefcaseProduct));
        
        tmp_non_faulty_products.erase(tmp_non_faulty_products.begin() + a);
        
        break;
      }
    }


     for (auto& product : mapOfBriefcaseProducts)
    {
      // +2pts if correct pose AND correct type
      if (product.second.isProductCorrectPose && product.second.isProductCorrectType)
      {
        product.second.productSuccess = 2;
        // +1pt if correct pose AND correct type AND correct color
        if (product.second.isProductCorrectColor)
        {
          product.second.productSuccess++;
        }
      }
    }

    unsigned nbOfSuccessfulProducts{ 0 };
    // scoring for all products bonus
    for (auto& product : mapOfBriefcaseProducts)
    {
      // +2pts if correct pose AND correct type
      if (product.second.productSuccess == 3)
      {
        nbOfSuccessfulProducts++;
      }
    }

    if (nbOfSuccessfulProducts == desired_shipment.products.size()) {
      scorer.allProductsBonus = 4 * nbOfSuccessfulProducts;
    }
    }  // end processing each part


  // // Map of product type to indexes in desired products (first) and indexes in non faulty actual products (second)
  // std::map<std::string, std::pair<std::vector<size_t>, std::vector<size_t>>> product_type_map;

  // for (size_t d = 0; d < desired_shipment.products.size(); ++d)
  // {
  //   const auto& desired_product = desired_shipment.products[d];
  //   auto& mapping = product_type_map[desired_product.type];
  //   mapping.first.push_back(d);
  // }
  // // gzerr << "4" << std::endl;

  // for (size_t a = 0; a < detected_non_faulty_products.size(); ++a)
  // {
  //   auto& actual_product = detected_non_faulty_products[a];

  //   if (0u == product_type_map.count(actual_product.type))
  //   {
  //     // since desired products were put into the type map first, this product must be unwanted
  //     scorer.hasUnwantedProduct = true;
  //     continue;
  //   }
  //   auto& mapping = product_type_map.at(actual_product.type);
  //   mapping.second.push_back(a);
  // }
  // // gzerr << "5" << std::endl;

  // for (const auto& type_pair : product_type_map)
  // {
  //   const std::vector<size_t>& desired_indexes = type_pair.second.first;
  //   const std::vector<size_t>& actual_indexes = type_pair.second.second;
  //   auto desired_product_name = type_pair.first;
  //   // ROS_WARN_STREAM("desired_indexes: "<< desired_indexes.size());
  //   // ROS_WARN_STREAM("actual_indexes: "<< actual_indexes.size());

  //   // products are missing
  //   if (desired_indexes.size() > actual_indexes.size())
  //   {
  //     scorer.hasMissingProduct = true;
  //   }
  //   // more products than needed
  //   else if (desired_indexes.size() < actual_indexes.size())
  //   {
  //     scorer.hasUnwantedProduct = true;
  //   }

  //   // no point in trying to score this type if there are none delivered
  //   if (actual_indexes.empty())
  //   {
  //     continue;
  //   }

  //   double contributing_pose_score = 0;
  //   size_t num_indices = std::max(desired_indexes.size(), actual_indexes.size());

  //   std::vector<size_t> permutation(num_indices);
  //   for (size_t i = 0; i < num_indices; ++i)
  //   {
  //     permutation[i] = i;
  //   }

  //   // Now iterate through all permutations of actual matched with desired to find the highest pose score
  //   ////////////////////////////////////
  //   // Check pose of desired vs. actual
  //   ////////////////////////////////////
  //   std::string briefcase_actual_product{};
  //   do
  //   {
  //     std::string ACTUAL_PRODUCT_TYPE;
  //     double permutation_pose_score = 0;
  //     for (size_t d = 0; d < desired_indexes.size(); ++d)
  //     {
  //       const size_t actual_index_index = permutation[d];
  //       if (actual_index_index >= actual_indexes.size())
  //       {
  //         // There were fewer actual products than the order called for
  //         continue;
  //       }
  //       const auto& desired_product = desired_shipment.products[desired_indexes[d]];
  //       const auto& actual_product = detected_non_faulty_products[actual_indexes[actual_index_index]];
  //       briefcase_actual_product = actual_product.type;
  //       // ROS_WARN_STREAM("desired_product: " << desired_product.type);

  //       //---------------------------------
  //       std::size_t pos = actual_product.type.find("assembly_");  // position of "assembly_" in actual_product_name
  //       // gzerr << "pos: " <<  pos << std::endl;
  //       std::string tmp = actual_product.type.substr(9);          // get from "assembly_" to the end
  //       // gzerr << "tmp: " <<  tmp << std::endl;
  //       // gzerr << "tmp: " << tmp << std::endl;
  //       pos = tmp.find("_");
  //       // gzerr << "pos: " <<  pos << std::endl;
  //       ACTUAL_PRODUCT_TYPE = tmp.substr(0, pos);
  //       gzerr << "Actual product type: " <<  ACTUAL_PRODUCT_TYPE << std::endl;
  //       //--------------------------------

  //       // ROS_WARN_STREAM("actual_product: " << ACTUALPRODUCTTYPE);

  //       // Add points for each product in the correct pose
  //       const double translation_target = 0.02;  // 2 cm
  //       const double orientation_target = 0.2;   // 0.1 rad
  //       // get translation distance
  //       ignition::math::Vector3d posnDiff(desired_product.pose.position.x - actual_product.pose.position.x,
  //                                         desired_product.pose.position.y - actual_product.pose.position.y, 0);
  //       // ROS_WARN_STREAM("Position -- desired_product.pose: " <<
  //       // desired_product.pose.position.x << ", " << desired_product.pose.position.y);
  //       // ROS_WARN_STREAM("Position -- actual_product.pose: " <<
  //       // actual_product.pose.position.x << ", " << actual_product.pose.position.y);

  //       const double distance = posnDiff.Length();
  //       // ROS_WARN_STREAM("Position -- distance: " << distance);

  //       if (distance > translation_target)
  //       {
  //         // Skipping product because translation error is too big
  //         // ROS_WARN_STREAM("p -- SKIPPING PRODUCT ");
  //         continue;
  //       }

  //       ignition::math::Quaterniond orderOrientation(
  //           desired_product.pose.orientation.w, desired_product.pose.orientation.x, desired_product.pose.orientation.y,
  //           desired_product.pose.orientation.z);

  //       ignition::math::Quaterniond objOrientation(actual_product.pose.orientation.w, actual_product.pose.orientation.x,
  //                                                  actual_product.pose.orientation.y,
  //                                                  actual_product.pose.orientation.z);

  //       // Filter products that aren't in the appropriate orientation (loosely).
  //       // If the quaternions represent the same orientation, q1 = +-q2 => q1.dot(q2) = +-1
  //       const double orientationDiff = objOrientation.Dot(orderOrientation);
  //       // TODO(zeid): this value can probably be derived using relationships between
  //       // euler angles and quaternions.
  //       const double quaternionDiffThresh = 0.05;

  //       // ROS_WARN_STREAM("Orientation -- desired_product: " <<
  //       // desired_product.pose.positoriention.x << ", " << desired_product.pose.position.y);
  //       // ROS_WARN_STREAM("Orientation -- actual_product: " <<
  //       // actual_product.pose.position.x << ", " << actual_product.pose.position.y);

  //       auto diff_orientation = std::abs(orientationDiff) < (1.0 - quaternionDiffThresh);
  //       // ROS_WARN_STREAM("orientationDiff: " << std::abs(orientationDiff));
  //       // ROS_WARN_STREAM("1.0 - quaternionDiffThresh: " << 1.0 - quaternionDiffThresh);

  //       if (std::abs(orientationDiff) < (1.0 - quaternionDiffThresh))
  //       {
  //         // ROS_WARN_STREAM("o -- SKIPPING PRODUCT ");
  //         // Skipping product because it is not in the correct orientation (roughly)
  //         continue;
  //       }

  //       permutation_pose_score += 1.0;

  //       // double angleDiff;
  //       // // For sensors and regulators, check the roll only.
  //       // if (ACTUALPRODUCTTYPE == "sensor" || ACTUALPRODUCTTYPE == "regulator")
  //       // {
  //       //   ROS_WARN_STREAM("----------------------------");
  //       //   // Filter the yaw based on a threshold set in radians (more user-friendly).
  //       //   // Account for wrapping in angles. E.g. -pi compared with pi should "pass".
  //       //   ROS_WARN_STREAM("current roll -- " << objOrientation.Roll());
  //       //   ROS_WARN_STREAM("expected roll -- " << orderOrientation.Roll());
  //       //   ROS_WARN_STREAM("-----------");
  //       //   angleDiff = objOrientation.Roll() - orderOrientation.Roll();
  //       //   ROS_WARN_STREAM("angleDiff -- " << angleDiff);
  //       //   ROS_WARN_STREAM("angleDiff - 2PI -- " << std::abs(std::abs(angleDiff) - 2 * M_PI));
  //       //   ROS_WARN_STREAM("orientation_target -- " << orientation_target);
  //       //   // ROS_WARN_STREAM("-----------");

  //       //   if ((std::abs(angleDiff) < orientation_target) ||
  //       //       (std::abs(std::abs(angleDiff) - 2 * M_PI) <= orientation_target))
  //       //   {
  //       //     permutation_pose_score += 1.0;
  //       //     ROS_WARN_STREAM("GOOD ROLL");
  //       //     ROS_WARN_STREAM("----------------------------");
  //       //   }
  //       //   else
  //       //   {
  //       //     ROS_WARN_STREAM("BAD ROLL");
  //       //     ROS_WARN_STREAM("----------------------------");
  //       //   }
  //       // }
  //       // else if (ACTUALPRODUCTTYPE == "battery" || ACTUALPRODUCTTYPE == "pump")
  //       // {
  //       //   ROS_WARN_STREAM("----------------------------");
  //       // // Filter the yaw based on a threshold set in radians (more user-friendly).
  //       // // Account for wrapping in angles. E.g. -pi compared with pi should "pass".
  //       // ROS_WARN_STREAM("current yaw -- " << objOrientation.Yaw());
  //       // ROS_WARN_STREAM("expected yaw -- " << orderOrientation.Yaw());
  //       // ROS_WARN_STREAM("-----------");
  //       // angleDiff = objOrientation.Yaw() - orderOrientation.Yaw();
  //       // ROS_WARN_STREAM("angleDiff -- " << angleDiff);
  //       // ROS_WARN_STREAM("angleDiff - 2PI -- " << std::abs(std::abs(angleDiff) - 2 * M_PI));
  //       // ROS_WARN_STREAM("orientation_target -- " << orientation_target);
  //       // // ROS_WARN_STREAM("-----------");

  //       // if ((std::abs(angleDiff) < orientation_target) ||
  //       //     (std::abs(std::abs(angleDiff) - 2 * M_PI) <= orientation_target))
  //       // {
  //       //   permutation_pose_score += 1.0;
  //       //   ROS_WARN_STREAM("GOOD Yaw");
  //       //   ROS_WARN_STREAM("----------------------------");
  //       // }
  //       // else
  //       // {
  //       //   ROS_WARN_STREAM("BAD Yaw");
  //       //   ROS_WARN_STREAM("----------------------------");
  //       // }
  //       // }
  //     }

  //     if (permutation_pose_score > contributing_pose_score)
  //     {
  //       contributing_pose_score = permutation_pose_score;

  //       if (contributing_pose_score > 0)
  //       {
          
          
  //         std::map<std::string, ariac::BriefcaseProduct>::iterator it =
  //           mapOfBriefcaseProducts.find(briefcase_actual_product);

  //         for (auto& product : mapOfBriefcaseProducts) {
  //           gzerr << "Desired type: " << product.second.productType << std::endl;
  //           if (product.second.productType == ACTUAL_PRODUCT_TYPE) {
  //             scorer.numberOfProductsWithCorrectPose++;
  //           }
  //         }

          
  //         // if (it != mapOfBriefcaseProducts.end())
  //         // {
  //         //   it->second.isProductCorrectPose = true;
  //         //   scorer.numberOfProductsWithCorrectPose++;
  //         // }
  //       }
  //     }
  //   } while (std::next_permutation(permutation.begin(), permutation.end()));

  //   // unsigned nbOfProductsInBriefcase = mapOfBriefcaseProducts.size();
  //   // scoring for each product
  //   for (auto& product : mapOfBriefcaseProducts)
  //   {
  //     // +2pts if correct pose AND correct type
  //     if (product.second.isProductCorrectPose && product.second.isProductCorrectType)
  //     {
  //       product.second.productSuccess = 2;
  //       // +1pt if correct pose AND correct type AND correct color
  //       if (product.second.isProductCorrectColor)
  //       {
  //         product.second.productSuccess++;
  //       }
  //     }
  //   }

  //   unsigned nbOfSuccessfulProducts{ 0 };
  //   // scoring for all products bonus
  //   for (auto& product : mapOfBriefcaseProducts)
  //   {
  //     // +2pts if correct pose AND correct type
  //     if (product.second.productSuccess == 3)
  //     {
  //       nbOfSuccessfulProducts++;
  //     }
  //   }

  //   if (nbOfSuccessfulProducts == desired_shipment.products.size())
  //     scorer.allProductsBonus = 4 * nbOfSuccessfulProducts;
  // }  // end processing each part

  // // gzerr << "6" << std::endl;

  // if (!scorer.hasMissingProduct)
  // {
  //   scorer.isShipmentComplete = true;
  // }

  scorer.briefcaseProducts = mapOfBriefcaseProducts;
  // // gzerr << "7" << std::endl;

  return scorer;
}
