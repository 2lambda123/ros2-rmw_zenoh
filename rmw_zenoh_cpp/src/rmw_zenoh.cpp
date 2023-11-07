// Copyright 2023 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <fastcdr/FastBuffer.h>
#include <fastcdr/Cdr.h>

#include <chrono>
#include <mutex>
#include <new>

#include "detail/guard_condition.hpp"
#include "detail/identifier.hpp"
#include "detail/message_type_support.hpp"
#include "detail/rmw_data_types.hpp"
#include "detail/serialization_format.hpp"
#include "detail/type_support_common.hpp"

#include "rcpputils/scope_exit.hpp"

#include "rcutils/env.h"
#include "rcutils/logging_macros.h"
#include "rcutils/strdup.h"
#include "rcutils/types.h"

#include "rmw/allocators.h"
#include "rmw/dynamic_message_type_support.h"
#include "rmw/error_handling.h"
#include "rmw/features.h"
#include "rmw/impl/cpp/macros.hpp"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/validate_namespace.h"
#include "rmw/validate_node_name.h"

extern "C"
{
//==============================================================================
/// Get the name of the rmw implementation being used
const char *
rmw_get_implementation_identifier(void)
{
  return rmw_zenoh_identifier;
}

//==============================================================================
/// Get the unique serialization format for this middleware.
const char *
rmw_get_serialization_format(void)
{
  return rmw_zenoh_serialization_format;
}

//==============================================================================
bool rmw_feature_supported(rmw_feature_t feature)
{
  switch (feature) {
    case RMW_FEATURE_MESSAGE_INFO_PUBLICATION_SEQUENCE_NUMBER:
      return false;
    case RMW_FEATURE_MESSAGE_INFO_RECEPTION_SEQUENCE_NUMBER:
      return false;
    case RMW_MIDDLEWARE_SUPPORTS_TYPE_DISCOVERY:
      return true;
    case RMW_MIDDLEWARE_CAN_TAKE_DYNAMIC_MESSAGE:
      return false;
    default:
      return false;
  }
}

//==============================================================================
/// Create a node and return a handle to that node.
rmw_node_t *
rmw_create_node(
  rmw_context_t * context,
  const char * name,
  const char * namespace_)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(context, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    context,
    context->implementation_identifier,
    rmw_zenoh_identifier,
    return nullptr);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    context->impl,
    "expected initialized context",
    return nullptr);
  if (context->impl->is_shutdown) {
    RCUTILS_SET_ERROR_MSG("context has been shutdown");
    return nullptr;
  }

  int validation_result = RMW_NODE_NAME_VALID;
  rmw_ret_t ret = rmw_validate_node_name(name, &validation_result, nullptr);
  if (RMW_RET_OK != ret) {
    return nullptr;
  }
  if (RMW_NODE_NAME_VALID != validation_result) {
    const char * reason = rmw_node_name_validation_result_string(validation_result);
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid node name: %s", reason);
    return nullptr;
  }

  validation_result = RMW_NAMESPACE_VALID;
  ret = rmw_validate_namespace(namespace_, &validation_result, nullptr);
  if (RMW_RET_OK != ret) {
    return nullptr;
  }
  if (RMW_NAMESPACE_VALID != validation_result) {
    const char * reason = rmw_node_name_validation_result_string(validation_result);
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid node namespace: %s", reason);
    return nullptr;
  }

  rcutils_allocator_t * allocator = &context->options.allocator;

  rmw_node_t * node =
    static_cast<rmw_node_t *>(allocator->zero_allocate(1, sizeof(rmw_node_t), allocator->state));
  RMW_CHECK_FOR_NULL_WITH_MSG(
    node,
    "unable to allocate memory for rmw_node_t",
    return nullptr);
  auto free_node = rcpputils::make_scope_exit(
    [node, allocator]() {
      allocator->deallocate(node, allocator->state);
    });

  size_t name_len = strlen(name);
  // We specifically don't use rcutils_strdup() here because we want to avoid iterating over the
  // name again looking for the \0 (we already did that above).
  char * new_string = static_cast<char *>(allocator->allocate(name_len + 1, allocator->state));
  RMW_CHECK_FOR_NULL_WITH_MSG(
    new_string,
    "unable to allocate memory for node name",
    return nullptr);
  memcpy(new_string, name, name_len);
  new_string[name_len] = '\0';
  node->name = new_string;
  auto free_name = rcpputils::make_scope_exit(
    [node, allocator]() {
      allocator->deallocate(const_cast<char *>(node->name), allocator->state);
    });

  node->namespace_ = rcutils_strdup(namespace_, *allocator);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    node->namespace_,
    "unable to allocate memory for node namespace",
    return nullptr);
  auto free_namespace = rcpputils::make_scope_exit(
    [node, allocator]() {
      allocator->deallocate(const_cast<char *>(node->namespace_), allocator->state);
    });

  // TODO(yadunund): Register with storage system here and throw error if
  // zenohd is not running.
  // Put metadata into node->data.
  node->data = allocator->zero_allocate(1, sizeof(rmw_node_data_t), allocator->state);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    node->data,
    "unable to allocate memory for node data",
    return nullptr);
  auto free_node_data = rcpputils::make_scope_exit(
    [node, allocator]() {
      allocator->deallocate(node->data, allocator->state);
    });

  node->implementation_identifier = rmw_zenoh_identifier;
  node->context = context;

  // Publish to the graph that a new node is in town
  z_publisher_put_options_t pub_options = z_publisher_put_options_default();
  pub_options.encoding = z_encoding(Z_ENCODING_PREFIX_EMPTY, NULL);
  z_publisher_put(
    z_loan(context->impl->graph_publisher),
    reinterpret_cast<const uint8_t *>(node->name),
    name_len,
    &pub_options);

  free_node_data.cancel();
  free_namespace.cancel();
  free_name.cancel();
  free_node.cancel();

  return node;
}

//==============================================================================
/// Finalize a given node handle, reclaim the resources, and deallocate the node handle.
rmw_ret_t
rmw_destroy_node(rmw_node_t * node)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    rmw_zenoh_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  // Publish to the graph that a node has ridden off into the sunset
  z_publisher_put_options_t pub_options = z_publisher_put_options_default();
  pub_options.encoding = z_encoding(Z_ENCODING_PREFIX_EMPTY, NULL);
  z_publisher_put(
    z_loan(node->context->impl->graph_publisher),
    reinterpret_cast<const uint8_t *>(node->name),
    strlen(node->name),
    &pub_options);

  rcutils_allocator_t * allocator = &node->context->options.allocator;

  allocator->deallocate(node->data, allocator->state);
  allocator->deallocate(const_cast<char *>(node->namespace_), allocator->state);
  allocator->deallocate(const_cast<char *>(node->name), allocator->state);
  allocator->deallocate(node, allocator->state);

  return RMW_RET_OK;
}

//==============================================================================
/// Return a guard condition which is triggered when the ROS graph changes.
const rmw_guard_condition_t *
rmw_node_get_graph_guard_condition(const rmw_node_t * node)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    rmw_zenoh_identifier,
    return nullptr);

  RMW_CHECK_ARGUMENT_FOR_NULL(node->context, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(node->context->impl, nullptr);

  return node->context->impl->graph_guard_condition;
}

//==============================================================================
/// Initialize a publisher allocation to be used with later publications.
rmw_ret_t
rmw_init_publisher_allocation(
  const rosidl_message_type_support_t * type_support,
  const rosidl_runtime_c__Sequence__bound * message_bounds,
  rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(type_support);
  static_cast<void>(message_bounds);
  static_cast<void>(allocation);
  RMW_SET_ERROR_MSG("rmw_init_publisher_allocation: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Destroy a publisher allocation object.
rmw_ret_t
rmw_fini_publisher_allocation(
  rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(allocation);
  RMW_SET_ERROR_MSG("rmw_fini_publisher_allocation: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

// A function to take ros topic names and convert them to valid Zenoh keys.
// In particular, Zenoh keys cannot start or end with a /, so this function
// will strip them out.
// Performance note: at present, this function allocates a new string and copies
// the old string into it.  If this becomes a performance problem, we could consider
// modifying the topic_name in place.  But this means we need to be much more
// careful about who owns the string.
static char * ros_topic_name_to_zenoh_key(
  const char * const topic_name, rcutils_allocator_t * allocator)
{
  size_t start_offset = 0;
  size_t topic_name_len = strlen(topic_name);
  size_t end_offset = topic_name_len;

  if (topic_name_len > 0) {
    if (topic_name[0] == '/') {
      // Strip the leading '/'
      start_offset = 1;
    }
    if (topic_name[end_offset - 1] == '/') {
      // Strip the trailing '/'
      end_offset -= 1;
    }
  }

  return rcutils_strndup(&topic_name[start_offset], end_offset - start_offset, *allocator);
}

static const rosidl_message_type_support_t * find_type_support(
  const rosidl_message_type_support_t * type_supports)
{
  const rosidl_message_type_support_t * type_support = get_message_typesupport_handle(
    type_supports, RMW_ZENOH_CPP_TYPESUPPORT_C);
  if (!type_support) {
    rcutils_error_string_t prev_error_string = rcutils_get_error_string();
    rcutils_reset_error();
    type_support = get_message_typesupport_handle(type_supports, RMW_ZENOH_CPP_TYPESUPPORT_CPP);
    if (!type_support) {
      rcutils_error_string_t error_string = rcutils_get_error_string();
      rcutils_reset_error();
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING(
        "Type support not from this implementation. Got:\n"
        "    %s\n"
        "    %s\n"
        "while fetching it",
        prev_error_string.str, error_string.str);
      return nullptr;
    }
  }

  return type_support;
}

//==============================================================================
/// Create a publisher and return a handle to that publisher.
rmw_publisher_t *
rmw_create_publisher(
  const rmw_node_t * node,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name,
  const rmw_qos_profile_t * qos_profile,
  const rmw_publisher_options_t * publisher_options)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    rmw_zenoh_identifier,
    return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(type_supports, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, nullptr);
  if (topic_name[0] == '\0') {
    RMW_SET_ERROR_MSG("topic_name argument is an empty string");
    return nullptr;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(qos_profile, nullptr);
  if (!qos_profile->avoid_ros_namespace_conventions) {
    int validation_result = RMW_TOPIC_VALID;
    rmw_ret_t ret = rmw_validate_full_topic_name(topic_name, &validation_result, nullptr);
    if (RMW_RET_OK != ret) {
      return nullptr;
    }
    if (RMW_TOPIC_VALID != validation_result) {
      const char * reason = rmw_full_topic_name_validation_result_string(validation_result);
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid topic name: %s", reason);
      return nullptr;
    }
  }
  // Adapt any 'best available' QoS options
  // rmw_qos_profile_t adapted_qos_profile = *qos_profile;
  // rmw_ret_t ret = rmw_dds_common::qos_profile_get_best_available_for_topic_publisher(
  //   node, topic_name, &adapted_qos_profile, rmw_get_subscriptions_info_by_topic);
  // if (RMW_RET_OK != ret) {
  //   return nullptr;
  // }
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher_options, nullptr);
  if (publisher_options->require_unique_network_flow_endpoints ==
    RMW_UNIQUE_NETWORK_FLOW_ENDPOINTS_STRICTLY_REQUIRED)
  {
    RMW_SET_ERROR_MSG(
      "Strict requirement on unique network flow endpoints for publishers not supported");
    return nullptr;
  }

  // Get the RMW type support.
  const rosidl_message_type_support_t * type_support = find_type_support(type_supports);
  if (type_support == nullptr) {
    // error was already set by find_type_support
    return nullptr;
  }

  RMW_CHECK_FOR_NULL_WITH_MSG(
    node->context,
    "expected initialized context",
    return nullptr);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    node->context->impl,
    "expected initialized context impl",
    return nullptr);
  rmw_context_impl_s * context_impl = static_cast<rmw_context_impl_s *>(
    node->context->impl);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    context_impl,
    "unable to get rmw_context_impl_s",
    return nullptr);
  if (!z_check(context_impl->session)) {
    RMW_SET_ERROR_MSG("zenoh session is invalid");
    return nullptr;
  }

  rcutils_allocator_t * allocator = &node->context->options.allocator;

  // Create the publisher.
  auto rmw_publisher =
    static_cast<rmw_publisher_t *>(allocator->zero_allocate(
      1, sizeof(rmw_publisher_t), allocator->state));
  RMW_CHECK_FOR_NULL_WITH_MSG(
    rmw_publisher,
    "failed to allocate memory for the publisher",
    return nullptr);
  auto free_rmw_publisher = rcpputils::make_scope_exit(
    [rmw_publisher, allocator]() {
      allocator->deallocate(rmw_publisher, allocator->state);
    });

  auto publisher_data = static_cast<rmw_publisher_data_t *>(
    allocator->allocate(sizeof(rmw_publisher_data_t), allocator->state));
  RMW_CHECK_FOR_NULL_WITH_MSG(
    publisher_data,
    "failed to allocate memory for publisher data",
    return nullptr);
  auto free_publisher_data = rcpputils::make_scope_exit(
    [publisher_data, allocator]() {
      allocator->deallocate(publisher_data, allocator->state);
    });

  publisher_data->typesupport_identifier = type_support->typesupport_identifier;
  publisher_data->type_support_impl = type_support->data;
  auto callbacks = static_cast<const message_type_support_callbacks_t *>(type_support->data);
  publisher_data->type_support = static_cast<MessageTypeSupport *>(
    allocator->allocate(sizeof(MessageTypeSupport), allocator->state));
  RMW_CHECK_FOR_NULL_WITH_MSG(
    publisher_data->type_support,
    "Failed to allocate MessageTypeSupport",
    return nullptr);
  auto free_type_support = rcpputils::make_scope_exit(
    [publisher_data, allocator]() {
      allocator->deallocate(publisher_data->type_support, allocator->state);
    });

  RMW_TRY_PLACEMENT_NEW(
    publisher_data->type_support,
    publisher_data->type_support,
    return nullptr,
    MessageTypeSupport, callbacks);
  auto destruct_msg_type_support = rcpputils::make_scope_exit(
    [publisher_data]() {
      RMW_TRY_DESTRUCTOR_FROM_WITHIN_FAILURE(
        publisher_data->type_support->~MessageTypeSupport(),
        MessageTypeSupport);
    });

  publisher_data->context = node->context;
  rmw_publisher->data = publisher_data;
  rmw_publisher->implementation_identifier = rmw_zenoh_identifier;
  rmw_publisher->options = *publisher_options;
  // TODO(yadunund): Update this.
  rmw_publisher->can_loan_messages = false;

  size_t topic_len = strlen(topic_name);
  // We specifically don't use rcutils_strdup() here because we want to avoid iterating over the
  // name again looking for the \0 (we already did that above).
  char * new_string = static_cast<char *>(allocator->allocate(topic_len + 1, allocator->state));
  RMW_CHECK_FOR_NULL_WITH_MSG(
    new_string,
    "Failed to allocate topic name",
    return nullptr);
  memcpy(new_string, topic_name, topic_len);
  new_string[topic_len] = '\0';
  rmw_publisher->topic_name = new_string;
  auto free_topic_name = rcpputils::make_scope_exit(
    [rmw_publisher, allocator]() {
      allocator->deallocate(const_cast<char *>(rmw_publisher->topic_name), allocator->state);
    });

  // TODO(yadunund): Parse adapted_qos_profile and publisher_options to generate
  // a z_publisher_put_options struct instead of passing NULL to this function.
  char * zenoh_key_name = ros_topic_name_to_zenoh_key(topic_name, allocator);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    zenoh_key_name,
    "failed to allocate memory for zenoh key name",
    return nullptr);
  // TODO(clalancette): What happens if the key name is a valid but empty string?
  publisher_data->pub = z_declare_publisher(
    z_loan(context_impl->session),
    z_keyexpr(zenoh_key_name),
    NULL
  );
  allocator->deallocate(zenoh_key_name, allocator->state);
  if (!z_check(publisher_data->pub)) {
    RMW_SET_ERROR_MSG("unable to create zenoh publisher");
    return nullptr;
  }
  auto undeclare_z_publisher = rcpputils::make_scope_exit(
    [publisher_data]() {
      z_undeclare_publisher(z_move(publisher_data->pub));
    });

  // Publish to the graph that a new publisher is in town
  z_publisher_put_options_t pub_options = z_publisher_put_options_default();
  pub_options.encoding = z_encoding(Z_ENCODING_PREFIX_EMPTY, NULL);
  z_publisher_put(
    z_loan(node->context->impl->graph_publisher),
    reinterpret_cast<const uint8_t *>(rmw_publisher->topic_name),
    topic_len,
    &pub_options);

  publisher_data->graph_cache_handle = node->context->impl->graph_cache.add_publisher(
    rmw_publisher->topic_name, node->name, node->namespace_,
    publisher_data->type_support->get_name(), allocator);
  auto remove_from_graph_cache = rcpputils::make_scope_exit(
    [node, publisher_data]() {
      node->context->impl->graph_cache.remove_publisher(publisher_data->graph_cache_handle);
    });

  remove_from_graph_cache.cancel();
  undeclare_z_publisher.cancel();
  free_topic_name.cancel();
  destruct_msg_type_support.cancel();
  free_type_support.cancel();
  free_publisher_data.cancel();
  free_rmw_publisher.cancel();

  return rmw_publisher;
}

//==============================================================================
/// Finalize a given publisher handle, reclaim the resources, and deallocate the publisher handle.
rmw_ret_t
rmw_destroy_publisher(rmw_node_t * node, rmw_publisher_t * publisher)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    rmw_zenoh_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher,
    publisher->implementation_identifier,
    rmw_zenoh_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  rmw_ret_t ret = RMW_RET_OK;

  // Publish to the graph that a publisher has ridden off into the sunset
  z_publisher_put_options_t pub_options = z_publisher_put_options_default();
  pub_options.encoding = z_encoding(Z_ENCODING_PREFIX_EMPTY, NULL);
  z_publisher_put(
    z_loan(node->context->impl->graph_publisher),
    reinterpret_cast<const uint8_t *>(publisher->topic_name),
    strlen(publisher->topic_name),
    &pub_options);

  rcutils_allocator_t * allocator = &node->context->options.allocator;

  allocator->deallocate(const_cast<char *>(publisher->topic_name), allocator->state);

  auto publisher_data = static_cast<rmw_publisher_data_t *>(publisher->data);
  if (publisher_data != nullptr) {
    node->context->impl->graph_cache.remove_publisher(publisher_data->graph_cache_handle);

    RMW_TRY_DESTRUCTOR(publisher_data->type_support->~MessageTypeSupport(), MessageTypeSupport, );
    allocator->deallocate(publisher_data->type_support, allocator->state);
    if (z_undeclare_publisher(z_move(publisher_data->pub))) {
      RMW_SET_ERROR_MSG("failed to undeclare pub");
      ret = RMW_RET_ERROR;
    }
    allocator->deallocate(publisher_data, allocator->state);
  }
  allocator->deallocate(publisher, allocator->state);

  return ret;
}

//==============================================================================
rmw_ret_t
rmw_take_dynamic_message(
  const rmw_subscription_t * subscription,
  rosidl_dynamic_typesupport_dynamic_data_t * dynamic_message,
  bool * taken,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(subscription);
  static_cast<void>(dynamic_message);
  static_cast<void>(taken);
  static_cast<void>(allocation);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
rmw_ret_t
rmw_take_dynamic_message_with_info(
  const rmw_subscription_t * subscription,
  rosidl_dynamic_typesupport_dynamic_data_t * dynamic_message,
  bool * taken,
  rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(subscription);
  static_cast<void>(dynamic_message);
  static_cast<void>(taken);
  static_cast<void>(message_info);
  static_cast<void>(allocation);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
rmw_ret_t
rmw_serialization_support_init(
  const char * serialization_lib_name,
  rcutils_allocator_t * allocator,
  rosidl_dynamic_typesupport_serialization_support_t * serialization_support)
{
  static_cast<void>(serialization_lib_name);
  static_cast<void>(allocator);
  static_cast<void>(serialization_support);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Borrow a loaned ROS message.
rmw_ret_t
rmw_borrow_loaned_message(
  const rmw_publisher_t * publisher,
  const rosidl_message_type_support_t * type_support,
  void ** ros_message)
{
  static_cast<void>(publisher);
  static_cast<void>(type_support);
  static_cast<void>(ros_message);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Return a loaned message previously borrowed from a publisher.
rmw_ret_t
rmw_return_loaned_message_from_publisher(
  const rmw_publisher_t * publisher,
  void * loaned_message)
{
  static_cast<void>(publisher);
  static_cast<void>(loaned_message);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Publish a ROS message.
rmw_ret_t
rmw_publish(
  const rmw_publisher_t * publisher,
  const void * ros_message,
  rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(allocation);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    publisher, "publisher handle is null",
    return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher, publisher->implementation_identifier, rmw_zenoh_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    ros_message, "ros message handle is null",
    return RMW_RET_INVALID_ARGUMENT);

  auto publisher_data = static_cast<rmw_publisher_data_t *>(publisher->data);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    publisher_data, "publisher_data is null",
    return RMW_RET_INVALID_ARGUMENT);

  rcutils_allocator_t * allocator = &(publisher_data->context->options.allocator);

  // Serialize data.
  size_t max_data_length = publisher_data->type_support->get_estimated_serialized_size(
    ros_message,
    publisher_data->type_support_impl);

  // To store serialized message byte array.
  char * msg_bytes = nullptr;
  bool from_shm = false;
  auto free_msg_bytes = rcpputils::make_scope_exit(
    [msg_bytes, allocator, from_shm]() {
      if (msg_bytes && !from_shm) {
        allocator->deallocate(msg_bytes, allocator->state);
      }
    });

  zc_owned_shmbuf_t shmbuf;
  // Get memory from SHM buffer if available.
  if (zc_shm_manager_check(&publisher_data->context->impl->shm_manager)) {
    shmbuf = zc_shm_alloc(
      &publisher_data->context->impl->shm_manager,
      max_data_length);
    if (!z_check(shmbuf)) {
      zc_shm_gc(&publisher_data->context->impl->shm_manager);
      shmbuf = zc_shm_alloc(&publisher_data->context->impl->shm_manager, max_data_length);
      if (!z_check(shmbuf)) {
        // TODO(Yadunund): Should we revert to regular allocation and not return an error?
        RMW_SET_ERROR_MSG("Failed to allocate a SHM buffer, even after GCing");
        return RMW_RET_ERROR;
      }
    }
    msg_bytes = (char *)zc_shmbuf_ptr(&shmbuf);
    from_shm = true;
  }
  // Get memory from the allocator.
  else {
    msg_bytes = static_cast<char *>(allocator->allocate(max_data_length, allocator->state));
    RMW_CHECK_FOR_NULL_WITH_MSG(
      msg_bytes, "bytes for message is null", return RMW_RET_BAD_ALLOC);
  }

  // Object that manages the raw buffer
  eprosima::fastcdr::FastBuffer fastbuffer(msg_bytes, max_data_length);

  // Object that serializes the data
  eprosima::fastcdr::Cdr ser(
    fastbuffer,
    eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
    eprosima::fastcdr::Cdr::DDS_CDR);
  if (!publisher_data->type_support->serialize_ros_message(
      ros_message,
      ser,
      publisher_data->type_support_impl))
  {
    RMW_SET_ERROR_MSG("could not serialize ROS message");
    return RMW_RET_ERROR;
  }

  const size_t data_length = ser.getSerializedDataLength();

  int ret;
  // The encoding is simply forwarded and is useful when key expressions in the
  // session use different encoding formats. In our case, all key expressions
  // will be encoded with CDR so it does not really matter.
  z_publisher_put_options_t options = z_publisher_put_options_default();
  options.encoding = z_encoding(Z_ENCODING_PREFIX_EMPTY, NULL);

  if (from_shm) {
    zc_shmbuf_set_length(&shmbuf, data_length);
    zc_owned_payload_t payload = zc_shmbuf_into_payload(z_move(shmbuf));
    ret = zc_publisher_put_owned(z_loan(publisher_data->pub), z_move(payload), &options);
  } else {
    // Returns 0 if success.
    ret = z_publisher_put(
      z_loan(publisher_data->pub),
      reinterpret_cast<const uint8_t *>(msg_bytes),
      data_length,
      &options);
  }
  if (ret) {
    RMW_SET_ERROR_MSG("unable to publish message");
    return RMW_RET_ERROR;
  }

  return RMW_RET_OK;
}

//==============================================================================
/// Publish a loaned ROS message.
rmw_ret_t
rmw_publish_loaned_message(
  const rmw_publisher_t * publisher,
  void * ros_message,
  rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(publisher);
  static_cast<void>(ros_message);
  static_cast<void>(allocation);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Retrieve the number of matched subscriptions to a publisher.
rmw_ret_t
rmw_publisher_count_matched_subscriptions(
  const rmw_publisher_t * publisher,
  size_t * subscription_count)
{
  static_cast<void>(publisher);
  static_cast<void>(subscription_count);
  // TODO(yadunund): Fixme.
  *subscription_count = 0;
  return RMW_RET_OK;
}

//==============================================================================
/// Retrieve the actual qos settings of the publisher.
rmw_ret_t
rmw_publisher_get_actual_qos(
  const rmw_publisher_t * publisher,
  rmw_qos_profile_t * qos)
{
  static_cast<void>(publisher);
  static_cast<void>(qos);
  // TODO(yadunund): Fixme.
  return RMW_RET_OK;
}

//==============================================================================
/// Publish a ROS message as a byte stream.
rmw_ret_t
rmw_publish_serialized_message(
  const rmw_publisher_t * publisher,
  const rmw_serialized_message_t * serialized_message,
  rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(allocation);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    publisher, "publisher handle is null",
    return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher, publisher->implementation_identifier, rmw_zenoh_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    serialized_message, "serialized message handle is null",
    return RMW_RET_INVALID_ARGUMENT);

  auto publisher_data = static_cast<rmw_publisher_data_t *>(publisher->data);
  RCUTILS_CHECK_FOR_NULL_WITH_MSG(
    publisher_data, "publisher data pointer is null",
    return RMW_RET_ERROR);

  eprosima::fastcdr::FastBuffer buffer(
    reinterpret_cast<char *>(serialized_message->buffer), serialized_message->buffer_length);
  eprosima::fastcdr::Cdr ser(
    buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
  if (!ser.jump(serialized_message->buffer_length)) {
    RMW_SET_ERROR_MSG("cannot correctly set serialized buffer");
    return RMW_RET_ERROR;
  }

  const size_t data_length = ser.getSerializedDataLength();

  // The encoding is simply forwarded and is useful when key expressions in the
  // session use different encoding formats. In our case, all key expressions
  // will be encoded with CDR so it does not really matter.
  z_publisher_put_options_t options = z_publisher_put_options_default();
  options.encoding = z_encoding(Z_ENCODING_PREFIX_EMPTY, NULL);
  // Returns 0 if success.
  int8_t ret = z_publisher_put(
    z_loan(publisher_data->pub),
    serialized_message->buffer,
    data_length,
    &options);

  if (ret) {
    RMW_SET_ERROR_MSG("unable to publish message");
    return RMW_RET_ERROR;
  }

  return RMW_RET_OK;
}

//==============================================================================
/// Compute the size of a serialized message.
rmw_ret_t
rmw_get_serialized_message_size(
  const rosidl_message_type_support_t * type_support,
  const rosidl_runtime_c__Sequence__bound * message_bounds,
  size_t * size)
{
  static_cast<void>(type_support);
  static_cast<void>(message_bounds);
  static_cast<void>(size);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Manually assert that this Publisher is alive (for RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC)
rmw_ret_t
rmw_publisher_assert_liveliness(const rmw_publisher_t * publisher)
{
  static_cast<void>(publisher);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Wait until all published message data is acknowledged or until the specified timeout elapses.
rmw_ret_t
rmw_publisher_wait_for_all_acked(
  const rmw_publisher_t * publisher,
  rmw_time_t wait_timeout)
{
  static_cast<void>(publisher);
  static_cast<void>(wait_timeout);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Serialize a ROS message into a rmw_serialized_message_t.
rmw_ret_t
rmw_serialize(
  const void * ros_message,
  const rosidl_message_type_support_t * type_support,
  rmw_serialized_message_t * serialized_message)
{
  const rosidl_message_type_support_t * ts = find_type_support(type_support);
  if (ts == nullptr) {
    // error was already set by find_type_support
    return RMW_RET_ERROR;
  }

  auto callbacks = static_cast<const message_type_support_callbacks_t *>(ts->data);
  auto tss = MessageTypeSupport(callbacks);
  auto data_length = tss.get_estimated_serialized_size(ros_message, callbacks);
  if (serialized_message->buffer_capacity < data_length) {
    if (rmw_serialized_message_resize(serialized_message, data_length) != RMW_RET_OK) {
      RMW_SET_ERROR_MSG("unable to dynamically resize serialized message");
      return RMW_RET_ERROR;
    }
  }

  eprosima::fastcdr::FastBuffer buffer(
    reinterpret_cast<char *>(serialized_message->buffer), data_length);
  eprosima::fastcdr::Cdr ser(
    buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);

  auto ret = tss.serialize_ros_message(ros_message, ser, callbacks);
  serialized_message->buffer_length = data_length;
  serialized_message->buffer_capacity = data_length;
  return ret == true ? RMW_RET_OK : RMW_RET_ERROR;
}

//==============================================================================
/// Deserialize a ROS message.
rmw_ret_t
rmw_deserialize(
  const rmw_serialized_message_t * serialized_message,
  const rosidl_message_type_support_t * type_support,
  void * ros_message)
{
  const rosidl_message_type_support_t * ts = find_type_support(type_support);
  if (ts == nullptr) {
    // error was already set by find_type_support
    return RMW_RET_ERROR;
  }

  auto callbacks = static_cast<const message_type_support_callbacks_t *>(ts->data);
  auto tss = MessageTypeSupport(callbacks);
  eprosima::fastcdr::FastBuffer buffer(
    reinterpret_cast<char *>(serialized_message->buffer), serialized_message->buffer_length);
  eprosima::fastcdr::Cdr deser(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
    eprosima::fastcdr::Cdr::DDS_CDR);

  auto ret = tss.deserialize_ros_message(deser, ros_message, callbacks);
  return ret == true ? RMW_RET_OK : RMW_RET_ERROR;
}

//==============================================================================
/// Initialize a subscription allocation to be used with later `take`s.
rmw_ret_t
rmw_init_subscription_allocation(
  const rosidl_message_type_support_t * type_support,
  const rosidl_runtime_c__Sequence__bound * message_bounds,
  rmw_subscription_allocation_t * allocation)
{
  // Unused in current implementation.
  static_cast<void>(type_support);
  static_cast<void>(message_bounds);
  static_cast<void>(allocation);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Destroy a publisher allocation object.
rmw_ret_t
rmw_fini_subscription_allocation(
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Create a subscription and return a handle to that subscription.
rmw_subscription_t *
rmw_create_subscription(
  const rmw_node_t * node,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name,
  const rmw_qos_profile_t * qos_profile,
  const rmw_subscription_options_t * subscription_options)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    rmw_zenoh_identifier,
    return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(type_supports, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, nullptr);
  if (topic_name[0] == '\0') {
    RMW_SET_ERROR_MSG("topic_name argument is an empty string");
    return nullptr;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(qos_profile, nullptr);
  // Adapt any 'best available' QoS options
  // rmw_qos_profile_t adapted_qos_profile = *qos_profile;
  // rmw_ret_t ret = rmw_dds_common::qos_profile_get_best_available_for_topic_subscription(
  //   node, topic_name, &adapted_qos_profile, rmw_get_publishers_info_by_topic);
  // if (RMW_RET_OK != ret) {
  //   return nullptr;
  // }
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription_options, nullptr);

  // Check RMW QoS
  // if (!is_valid_qos(*qos_profile)) {
  //   RMW_SET_ERROR_MSG("create_subscription() called with invalid QoS");
  //   return nullptr;
  // }

  const rosidl_message_type_support_t * type_support = find_type_support(type_supports);
  if (type_support == nullptr) {
    // error was already set by find_type_support
    return nullptr;
  }

  auto node_data = static_cast<rmw_node_data_t *>(node->data);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    node_data, "unable to create subscription as node_data is invalid.",
    return nullptr);
  // TODO(yadunund): Check if a duplicate entry for the same topic name + topic type
  // is present in node_data->subscriptions and if so return error;
  RMW_CHECK_FOR_NULL_WITH_MSG(
    node->context,
    "expected initialized context",
    return nullptr);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    node->context->impl,
    "expected initialized context impl",
    return nullptr);
  rmw_context_impl_s * context_impl = static_cast<rmw_context_impl_s *>(
    node->context->impl);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    context_impl,
    "unable to get rmw_context_impl_s",
    return nullptr);
  if (!z_check(context_impl->session)) {
    RMW_SET_ERROR_MSG("zenoh session is invalid");
    return nullptr;
  }

  rcutils_allocator_t * allocator = &node->context->options.allocator;

  // Create the rmw_subscription.
  rmw_subscription_t * rmw_subscription =
    static_cast<rmw_subscription_t *>(allocator->zero_allocate(
      1, sizeof(rmw_subscription_t), allocator->state));
  RMW_CHECK_FOR_NULL_WITH_MSG(
    rmw_subscription,
    "failed to allocate memory for the subscription",
    return nullptr);
  auto free_rmw_subscription = rcpputils::make_scope_exit(
    [rmw_subscription, allocator]() {
      allocator->deallocate(rmw_subscription, allocator->state);
    });

  auto sub_data = static_cast<rmw_subscription_data_t *>(
    allocator->allocate(sizeof(rmw_subscription_data_t), allocator->state));
  RMW_CHECK_FOR_NULL_WITH_MSG(
    sub_data,
    "failed to allocate memory for subscription data",
    return nullptr);
  auto free_sub_data = rcpputils::make_scope_exit(
    [sub_data, allocator]() {
      allocator->deallocate(sub_data, allocator->state);
    });

  RMW_TRY_PLACEMENT_NEW(sub_data, sub_data, return nullptr, rmw_subscription_data_t);
  auto destruct_sub_data = rcpputils::make_scope_exit(
    [sub_data]() {
      RMW_TRY_DESTRUCTOR_FROM_WITHIN_FAILURE(
        sub_data->~rmw_subscription_data_t(),
        rmw_subscription_data_t);
    });

  // Set the reliability of the subscription options based on qos_profile.
  // The default options will be initialized with Best Effort reliability.
  auto sub_options = z_subscriber_options_default();
  sub_data->reliable = false;
  if (qos_profile->reliability == RMW_QOS_POLICY_RELIABILITY_RELIABLE) {
    sub_options.reliability = Z_RELIABILITY_RELIABLE;
    sub_data->reliable = true;
  }

  sub_data->typesupport_identifier = type_support->typesupport_identifier;
  sub_data->type_support_impl = type_support->data;
  auto callbacks = static_cast<const message_type_support_callbacks_t *>(type_support->data);
  sub_data->type_support = static_cast<MessageTypeSupport *>(
    allocator->allocate(sizeof(MessageTypeSupport), allocator->state));
  RMW_CHECK_FOR_NULL_WITH_MSG(
    sub_data->type_support,
    "Failed to allocate MessageTypeSupport",
    return nullptr);
  auto free_type_support = rcpputils::make_scope_exit(
    [sub_data, allocator]() {
      allocator->deallocate(sub_data->type_support, allocator->state);
    });

  RMW_TRY_PLACEMENT_NEW(
    sub_data->type_support,
    sub_data->type_support,
    return nullptr,
    MessageTypeSupport, callbacks);
  auto destruct_msg_type_support = rcpputils::make_scope_exit(
    [sub_data]() {
      RMW_TRY_DESTRUCTOR_FROM_WITHIN_FAILURE(
        sub_data->type_support->~MessageTypeSupport(),
        MessageTypeSupport);
    });

  sub_data->queue_depth = qos_profile->depth;
  sub_data->context = node->context;

  rmw_subscription->implementation_identifier = rmw_zenoh_identifier;
  rmw_subscription->data = sub_data;

  size_t topic_len = strlen(topic_name);
  // We specifically don't use rcutils_strdup() here because we want to avoid iterating over the
  // name again looking for the \0 (we already did that above).
  char * new_string = static_cast<char *>(allocator->allocate(topic_len + 1, allocator->state));
  RMW_CHECK_FOR_NULL_WITH_MSG(
    new_string,
    "Failed to allocate topic name",
    return nullptr);
  memcpy(new_string, topic_name, topic_len);
  new_string[topic_len] = '\0';
  rmw_subscription->topic_name = new_string;
  auto free_topic_name = rcpputils::make_scope_exit(
    [rmw_subscription, allocator]() {
      allocator->deallocate(const_cast<char *>(rmw_subscription->topic_name), allocator->state);
    });

  rmw_subscription->options = *subscription_options;
  rmw_subscription->can_loan_messages = false;
  rmw_subscription->is_cft_enabled = false;

  // Everything above succeeded and is setup properly.  Now declare a subscriber
  // with Zenoh; after this, callbacks may come in at any time.

  z_owned_closure_sample_t callback = z_closure(sub_data_handler, nullptr, sub_data);
  char * zenoh_key_name = ros_topic_name_to_zenoh_key(topic_name, allocator);
  // TODO(clalancette): What happens if the key name is a valid but empty string?
  RMW_CHECK_FOR_NULL_WITH_MSG(
    zenoh_key_name,
    "failed to allocate memory for zenoh key name",
    return nullptr);
  sub_data->sub = z_declare_subscriber(
    z_loan(context_impl->session),
    z_keyexpr(zenoh_key_name),
    z_move(callback),
    &sub_options
  );
  allocator->deallocate(zenoh_key_name, allocator->state);
  if (!z_check(sub_data->sub)) {
    RMW_SET_ERROR_MSG("unable to create zenoh subscription");
    return nullptr;
  }
  auto undeclare_z_sub = rcpputils::make_scope_exit(
    [sub_data]() {
      z_undeclare_subscriber(z_move(sub_data->sub));
    });

  // Publish to the graph that a new subscription is in town
  z_publisher_put_options_t pub_options = z_publisher_put_options_default();
  pub_options.encoding = z_encoding(Z_ENCODING_PREFIX_EMPTY, NULL);
  z_publisher_put(
    z_loan(node->context->impl->graph_publisher),
    reinterpret_cast<const uint8_t *>(rmw_subscription->topic_name),
    topic_len,
    &pub_options);

  sub_data->graph_cache_handle = node->context->impl->graph_cache.add_subscription(
    rmw_subscription->topic_name, node->name, node->namespace_,
    sub_data->type_support->get_name(), allocator);
  auto remove_from_graph_cache = rcpputils::make_scope_exit(
    [node, sub_data]() {
      node->context->impl->graph_cache.remove_subscription(sub_data->graph_cache_handle);
    });

  remove_from_graph_cache.cancel();
  undeclare_z_sub.cancel();
  free_topic_name.cancel();
  destruct_msg_type_support.cancel();
  free_type_support.cancel();
  destruct_sub_data.cancel();
  free_sub_data.cancel();
  free_rmw_subscription.cancel();

  return rmw_subscription;
}

//==============================================================================
/// Finalize a given subscription handle, reclaim the resources, and deallocate the subscription
rmw_ret_t
rmw_destroy_subscription(rmw_node_t * node, rmw_subscription_t * subscription)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    rmw_zenoh_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription,
    subscription->implementation_identifier,
    rmw_zenoh_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  rmw_ret_t ret = RMW_RET_OK;

  // Publish to the graph that a subscription has ridden off into the sunset
  z_publisher_put_options_t pub_options = z_publisher_put_options_default();
  pub_options.encoding = z_encoding(Z_ENCODING_PREFIX_EMPTY, NULL);
  z_publisher_put(
    z_loan(node->context->impl->graph_publisher),
    reinterpret_cast<const uint8_t *>(subscription->topic_name),
    strlen(subscription->topic_name),
    &pub_options);

  rcutils_allocator_t * allocator = &node->context->options.allocator;

  allocator->deallocate(const_cast<char *>(subscription->topic_name), allocator->state);

  auto sub_data = static_cast<rmw_subscription_data_t *>(subscription->data);
  if (sub_data != nullptr) {
    node->context->impl->graph_cache.remove_subscription(sub_data->graph_cache_handle);

    RMW_TRY_DESTRUCTOR(sub_data->type_support->~MessageTypeSupport(), MessageTypeSupport, );
    allocator->deallocate(sub_data->type_support, allocator->state);

    if (z_undeclare_subscriber(z_move(sub_data->sub))) {
      RMW_SET_ERROR_MSG("failed to undeclare sub");
      ret = RMW_RET_ERROR;
    }

    RMW_TRY_DESTRUCTOR(sub_data->~rmw_subscription_data_t(), rmw_subscription_data_t, );
    allocator->deallocate(sub_data, allocator->state);
  }
  allocator->deallocate(subscription, allocator->state);

  return ret;
}

//==============================================================================
/// Retrieve the number of matched publishers to a subscription.
rmw_ret_t
rmw_subscription_count_matched_publishers(
  const rmw_subscription_t * subscription,
  size_t * publisher_count)
{
  static_cast<void>(subscription);

  // TODO(clalancette): implement
  *publisher_count = 0;

  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Retrieve the actual qos settings of the subscription.
rmw_ret_t
rmw_subscription_get_actual_qos(
  const rmw_subscription_t * subscription,
  rmw_qos_profile_t * qos)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription,
    subscription->implementation_identifier,
    rmw_zenoh_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  auto sub_data = static_cast<rmw_subscription_data_t *>(subscription->data);
  RMW_CHECK_ARGUMENT_FOR_NULL(sub_data, RMW_RET_INVALID_ARGUMENT);

  qos->reliability = sub_data->reliable ? RMW_QOS_POLICY_RELIABILITY_RELIABLE :
    RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
  return RMW_RET_OK;
}

//==============================================================================
/// Set the content filter options for the subscription.
rmw_ret_t
rmw_subscription_set_content_filter(
  rmw_subscription_t * subscription,
  const rmw_subscription_content_filter_options_t * options)
{
  static_cast<void>(subscription);
  static_cast<void>(options);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Retrieve the content filter options of the subscription.
rmw_ret_t
rmw_subscription_get_content_filter(
  const rmw_subscription_t * subscription,
  rcutils_allocator_t * allocator,
  rmw_subscription_content_filter_options_t * options)
{
  static_cast<void>(subscription);
  static_cast<void>(allocator);
  static_cast<void>(options);
  return RMW_RET_UNSUPPORTED;
}

static rmw_ret_t __rmw_take(
  const rmw_subscription_t * subscription,
  void * ros_message,
  bool * taken,
  rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription->topic_name, RMW_RET_ERROR);
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription->data, RMW_RET_ERROR);
  RMW_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(message_info, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription handle,
    subscription->implementation_identifier, rmw_zenoh_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  static_cast<void>(allocation);

  *taken = false;

  auto sub_data = static_cast<rmw_subscription_data_t *>(subscription->data);
  RMW_CHECK_ARGUMENT_FOR_NULL(sub_data, RMW_RET_INVALID_ARGUMENT);

  // RETRIEVE SERIALIZED MESSAGE ===============================================

  std::unique_ptr<saved_msg_data> msg_data;
  {
    std::unique_lock<std::mutex> lock(sub_data->message_queue_mutex);

    if (sub_data->message_queue.empty()) {
      // This tells rcl that the check for a new message was done, but no messages have come in yet.
      return RMW_RET_OK;
    }

    // NOTE(CH3): Potential place to handle "QoS" (e.g. could pop from back so it is LIFO)
    msg_data = std::move(sub_data->message_queue.back());
    sub_data->message_queue.pop_back();
  }

  // Object that manages the raw buffer
  eprosima::fastcdr::FastBuffer fastbuffer(
    reinterpret_cast<char *>(const_cast<uint8_t *>(msg_data->payload.payload.start)),
    msg_data->payload.payload.len);

  // Object that serializes the data
  eprosima::fastcdr::Cdr deser(
    fastbuffer,
    eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
    eprosima::fastcdr::Cdr::DDS_CDR);
  if (!sub_data->type_support->deserialize_ros_message(
      deser,
      ros_message,
      sub_data->type_support_impl))
  {
    RMW_SET_ERROR_MSG("could not deserialize ROS message");
    return RMW_RET_ERROR;
  }

  *taken = true;
  z_drop(&msg_data->payload);

  // TODO(clalancette): fill in source_timestamp
  message_info->source_timestamp = 0;
  message_info->received_timestamp = msg_data->recv_timestamp;
  // TODO(clalancette): fill in publication_sequence_number
  message_info->publication_sequence_number = 0;
  // TODO(clalancette): fill in reception_sequence_number
  message_info->reception_sequence_number = 0;
  message_info->publisher_gid.implementation_identifier = rmw_zenoh_identifier;
  memcpy(message_info->publisher_gid.data, msg_data->publisher_gid, 16);
  message_info->from_intra_process = false;

  return RMW_RET_OK;
}

//==============================================================================
/// Take an incoming ROS message.
rmw_ret_t
rmw_take(
  const rmw_subscription_t * subscription,
  void * ros_message,
  bool * taken,
  rmw_subscription_allocation_t * allocation)
{
  rmw_message_info_t dummy_msg_info;

  return __rmw_take(subscription, ros_message, taken, &dummy_msg_info, allocation);
}

//==============================================================================
/// Take an incoming ROS message with its metadata.
rmw_ret_t
rmw_take_with_info(
  const rmw_subscription_t * subscription,
  void * ros_message,
  bool * taken,
  rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  return __rmw_take(subscription, ros_message, taken, message_info, allocation);
}

//==============================================================================
/// Take multiple incoming ROS messages with their metadata.
rmw_ret_t
rmw_take_sequence(
  const rmw_subscription_t * subscription,
  size_t count,
  rmw_message_sequence_t * message_sequence,
  rmw_message_info_sequence_t * message_info_sequence,
  size_t * taken,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(subscription);
  static_cast<void>(count);
  static_cast<void>(message_sequence);
  static_cast<void>(message_info_sequence);
  static_cast<void>(taken);
  static_cast<void>(allocation);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Take an incoming ROS message as a byte stream.
rmw_ret_t
rmw_take_serialized_message(
  const rmw_subscription_t * subscription,
  rmw_serialized_message_t * serialized_message,
  bool * taken,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(subscription);
  static_cast<void>(serialized_message);
  static_cast<void>(taken);
  static_cast<void>(allocation);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Take an incoming ROS message as a byte stream with its metadata.
rmw_ret_t
rmw_take_serialized_message_with_info(
  const rmw_subscription_t * subscription,
  rmw_serialized_message_t * serialized_message,
  bool * taken,
  rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(subscription);
  static_cast<void>(serialized_message);
  static_cast<void>(taken);
  static_cast<void>(message_info);
  static_cast<void>(allocation);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Take an incoming ROS message, loaned by the middleware.
rmw_ret_t
rmw_take_loaned_message(
  const rmw_subscription_t * subscription,
  void ** loaned_message,
  bool * taken,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(subscription);
  static_cast<void>(loaned_message);
  static_cast<void>(taken);
  static_cast<void>(allocation);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Take a loaned message and with its additional message information.
rmw_ret_t
rmw_take_loaned_message_with_info(
  const rmw_subscription_t * subscription,
  void ** loaned_message,
  bool * taken,
  rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(subscription);
  static_cast<void>(loaned_message);
  static_cast<void>(taken);
  static_cast<void>(message_info);
  static_cast<void>(allocation);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Return a loaned ROS message previously taken from a subscription.
rmw_ret_t
rmw_return_loaned_message_from_subscription(
  const rmw_subscription_t * subscription,
  void * loaned_message)
{
  static_cast<void>(subscription);
  static_cast<void>(loaned_message);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Create a service client that can send requests to and receive replies from a service server.
rmw_client_t *
rmw_create_client(
  const rmw_node_t * node,
  const rosidl_service_type_support_t * type_support,
  const char * service_name,
  const rmw_qos_profile_t * qos_profile)
{
  static_cast<void>(node);
  static_cast<void>(type_support);
  static_cast<void>(service_name);
  static_cast<void>(qos_profile);
  return nullptr;
}

//==============================================================================
/// Destroy and unregister a service client from its node.
rmw_ret_t
rmw_destroy_client(rmw_node_t * node, rmw_client_t * client)
{
  static_cast<void>(node);
  static_cast<void>(client);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Send a ROS service request.
rmw_ret_t
rmw_send_request(
  const rmw_client_t * client,
  const void * ros_request,
  int64_t * sequence_id)
{
  static_cast<void>(client);
  static_cast<void>(ros_request);
  static_cast<void>(sequence_id);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Take an incoming ROS service response.
rmw_ret_t
rmw_take_response(
  const rmw_client_t * client,
  rmw_service_info_t * request_header,
  void * ros_response,
  bool * taken)
{
  static_cast<void>(client);
  static_cast<void>(request_header);
  static_cast<void>(ros_response);
  static_cast<void>(taken);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Retrieve the actual qos settings of the client's request publisher.
rmw_ret_t
rmw_client_request_publisher_get_actual_qos(
  const rmw_client_t * client,
  rmw_qos_profile_t * qos)
{
  static_cast<void>(client);
  static_cast<void>(qos);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Retrieve the actual qos settings of the client's response subscription.
rmw_ret_t
rmw_client_response_subscription_get_actual_qos(
  const rmw_client_t * client,
  rmw_qos_profile_t * qos)
{
  static_cast<void>(client);
  static_cast<void>(qos);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Create a service server that can receive requests from and send replies to a service client.
rmw_service_t *
rmw_create_service(
  const rmw_node_t * node,
  const rosidl_service_type_support_t * type_supports,
  const char * service_name,
  const rmw_qos_profile_t * qos_profiles)
{
  // Interim implementation to suppress type_description service that spins up
  // with a node by default.
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    rmw_zenoh_identifier,
    return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(type_supports, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(service_name, nullptr);
  if (0 == strlen(service_name)) {
    RMW_SET_ERROR_MSG("service_name argument is an empty string");
    return nullptr;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(qos_profiles, nullptr);
  if (!qos_profiles->avoid_ros_namespace_conventions) {
    int validation_result = RMW_TOPIC_VALID;
    rmw_ret_t ret = rmw_validate_full_topic_name(service_name, &validation_result, nullptr);
    if (RMW_RET_OK != ret) {
      return nullptr;
    }
    if (RMW_TOPIC_VALID != validation_result) {
      const char * reason = rmw_full_topic_name_validation_result_string(validation_result);
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("service_name argument is invalid: %s", reason);
      return nullptr;
    }
  }
  // rmw_qos_profile_t adapted_qos_profile =
  //   rmw_dds_common::qos_profile_update_best_available_for_services(*qos_profile);
  // if (!is_valid_qos(adapted_qos_profile)) {
  //   RMW_SET_ERROR_MSG("create_service() called with invalid QoS");
  //   return nullptr;
  // }

  rmw_service_t * service = rmw_service_allocate();
  if (!service) {
    RMW_SET_ERROR_MSG("failed to allocate rmw_service_t");
    return nullptr;
  }

  return service;
}

//==============================================================================
/// Destroy and unregister a service server from its node.
rmw_ret_t
rmw_destroy_service(rmw_node_t * node, rmw_service_t * service)
{
  // Interim implementation to suppress type_description service that spins up
  // with a node by default.
  if (node == nullptr || service == nullptr) {
    return RMW_RET_ERROR;
  }
  rmw_service_free(service);
  return RMW_RET_OK;
}

//==============================================================================
/// Take an incoming ROS service request.
rmw_ret_t
rmw_take_request(
  const rmw_service_t * service,
  rmw_service_info_t * request_header,
  void * ros_request,
  bool * taken)
{
  static_cast<void>(service);
  static_cast<void>(request_header);
  static_cast<void>(ros_request);
  static_cast<void>(taken);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Send a ROS service response.
rmw_ret_t
rmw_send_response(
  const rmw_service_t * service,
  rmw_request_id_t * request_header,
  void * ros_response)
{
  static_cast<void>(service);
  static_cast<void>(request_header);
  static_cast<void>(ros_response);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Retrieve the actual qos settings of the service's request subscription.
rmw_ret_t
rmw_service_request_subscription_get_actual_qos(
  const rmw_service_t * service,
  rmw_qos_profile_t * qos)
{
  // TODO(yadunund): Fix.
  static_cast<void>(service);
  static_cast<void>(qos);
  return RMW_RET_OK;
}

//==============================================================================
/// Retrieve the actual qos settings of the service's response publisher.
rmw_ret_t
rmw_service_response_publisher_get_actual_qos(
  const rmw_service_t * service,
  rmw_qos_profile_t * qos)
{
  // TODO(yadunund): Fix.
  static_cast<void>(service);
  static_cast<void>(qos);
  return RMW_RET_OK;
}

//==============================================================================
/// Create a guard condition and return a handle to that guard condition.
rmw_guard_condition_t *
rmw_create_guard_condition(rmw_context_t * context)
{
  rcutils_allocator_t * allocator = &context->options.allocator;

  auto guard_condition =
    static_cast<rmw_guard_condition_t *>(allocator->zero_allocate(
      1, sizeof(rmw_guard_condition_t), allocator->state));
  RMW_CHECK_FOR_NULL_WITH_MSG(
    guard_condition,
    "unable to allocate memory for guard_condition",
    return nullptr);
  auto free_guard_condition = rcpputils::make_scope_exit(
    [guard_condition, allocator]() {
      allocator->deallocate(guard_condition, allocator->state);
    });

  guard_condition->implementation_identifier = rmw_zenoh_identifier;
  guard_condition->context = context;

  guard_condition->data = allocator->zero_allocate(1, sizeof(GuardCondition), allocator->state);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    guard_condition->data,
    "unable to allocate memory for guard condition data",
    return nullptr);
  auto free_guard_condition_data = rcpputils::make_scope_exit(
    [guard_condition, allocator]() {
      allocator->deallocate(guard_condition->data, allocator->state);
    });

  new(guard_condition->data) GuardCondition;
  auto destruct_guard_condition = rcpputils::make_scope_exit(
    [guard_condition]() {
      RMW_TRY_DESTRUCTOR_FROM_WITHIN_FAILURE(
        static_cast<GuardCondition *>(guard_condition->data)->~GuardCondition(), GuardCondition);
    });

  destruct_guard_condition.cancel();
  free_guard_condition_data.cancel();
  free_guard_condition.cancel();

  return guard_condition;
}

/// Finalize a given guard condition handle, reclaim the resources, and deallocate the handle.
rmw_ret_t
rmw_destroy_guard_condition(rmw_guard_condition_t * guard_condition)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(guard_condition, RMW_RET_INVALID_ARGUMENT);

  rcutils_allocator_t * allocator = &guard_condition->context->options.allocator;

  if (guard_condition->data) {
    static_cast<GuardCondition *>(guard_condition->data)->~GuardCondition();
    allocator->deallocate(guard_condition->data, allocator->state);
  }

  allocator->deallocate(guard_condition, allocator->state);

  return RMW_RET_OK;
}

//==============================================================================
rmw_ret_t
rmw_trigger_guard_condition(const rmw_guard_condition_t * guard_condition)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(guard_condition, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    guard_condition,
    guard_condition->implementation_identifier,
    rmw_zenoh_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  static_cast<GuardCondition *>(guard_condition->data)->trigger();

  return RMW_RET_OK;
}

//==============================================================================
/// Create a wait set to store conditions that the middleware can wait on.
rmw_wait_set_t *
rmw_create_wait_set(rmw_context_t * context, size_t max_conditions)
{
  static_cast<void>(max_conditions);

  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, NULL);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    context,
    context->implementation_identifier,
    rmw_zenoh_identifier,
    return nullptr);

  rcutils_allocator_t * allocator = &context->options.allocator;

  auto wait_set = static_cast<rmw_wait_set_t *>(
    allocator->zero_allocate(1, sizeof(rmw_wait_set_t), allocator->state));
  RMW_CHECK_FOR_NULL_WITH_MSG(
    wait_set,
    "failed to allocate wait set",
    return nullptr);
  auto cleanup_wait_set = rcpputils::make_scope_exit(
    [wait_set, allocator]() {
      allocator->deallocate(wait_set, allocator->state);
    });

  wait_set->implementation_identifier = rmw_zenoh_identifier;

  wait_set->data = allocator->zero_allocate(1, sizeof(rmw_wait_set_data_t), allocator->state);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    wait_set->data,
    "failed to allocate wait set data",
    return nullptr);
  auto free_wait_set_data = rcpputils::make_scope_exit(
    [wait_set, allocator]() {
      allocator->deallocate(wait_set->data, allocator->state);
    });

  // Invoke placement new
  new(wait_set->data) rmw_wait_set_data_t;
  auto destruct_rmw_wait_set_data = rcpputils::make_scope_exit(
    [wait_set]() {
      RMW_TRY_DESTRUCTOR_FROM_WITHIN_FAILURE(
        static_cast<rmw_wait_set_data_t *>(wait_set->data)->~rmw_wait_set_data_t(),
        rmw_wait_set_data);
    });

  static_cast<rmw_wait_set_data_t *>(wait_set->data)->context = context;

  destruct_rmw_wait_set_data.cancel();
  free_wait_set_data.cancel();
  cleanup_wait_set.cancel();

  return wait_set;
}

//==============================================================================
/// Destroy a wait set.
rmw_ret_t
rmw_destroy_wait_set(rmw_wait_set_t * wait_set)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(wait_set, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(wait_set->data, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    wait_set,
    wait_set->implementation_identifier,
    rmw_zenoh_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  auto wait_set_data = static_cast<rmw_wait_set_data_t *>(wait_set->data);

  rcutils_allocator_t * allocator = &wait_set_data->context->options.allocator;

  wait_set_data->~rmw_wait_set_data_t();
  allocator->deallocate(wait_set_data, allocator->state);

  allocator->deallocate(wait_set, allocator->state);

  return RMW_RET_OK;
}

//==============================================================================
/// Waits on sets of different entities and returns when one is ready.
rmw_ret_t
rmw_wait(
  rmw_subscriptions_t * subscriptions,
  rmw_guard_conditions_t * guard_conditions,
  rmw_services_t * services,
  rmw_clients_t * clients,
  rmw_events_t * events,
  rmw_wait_set_t * wait_set,
  const rmw_time_t * wait_timeout)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(wait_set, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    wait set handle,
    wait_set->implementation_identifier, rmw_zenoh_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  // TODO(yadunund): Switch to debug log level.
  RCUTILS_LOG_WARN_NAMED(
    "rmw_zenoh_cpp",
    "[rmw_wait] %ld subscriptions, %ld services, %ld clients, %ld events, %ld guard conditions",
    subscriptions->subscriber_count,
    services->service_count,
    clients->client_count,
    events->event_count,
    guard_conditions->guard_condition_count);

  // TODO(yadunund): Switch to debug log level.
  if (wait_timeout) {
    RCUTILS_LOG_WARN_NAMED(
      "rmw_zenoh_common_cpp", "[rmw_wait] TIMEOUT: %ld s %ld ns",
      wait_timeout->sec,
      wait_timeout->nsec);
  }

  auto wait_set_data = static_cast<rmw_wait_set_data_t *>(wait_set->data);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    wait_set_data,
    "waitset data struct is null",
    return RMW_RET_ERROR);

  if (guard_conditions) {
    // Go through each of the guard conditions, and attach the wait set condition variable to them.
    // That way they can wake it up if they are triggered while we are waiting.
    for (size_t i = 0; i < guard_conditions->guard_condition_count; ++i) {
      // This is hard to track down, but each of the (void *) pointers in
      // guard_conditions->guard_conditions points to the data field of the related
      // rmw_guard_condition_t.  So we can directly cast it to GuardCondition.
      GuardCondition * gc = static_cast<GuardCondition *>(guard_conditions->guard_conditions[i]);
      if (gc != nullptr) {
        gc->attach_condition(&wait_set_data->condition_variable);
      }
    }
  }

  if (subscriptions) {
    // Go through each of the subscriptions and attach the wait set condition variable to them.
    // That way they can wake it up if they are triggered while we are waiting.
    for (size_t i = 0; i < subscriptions->subscriber_count; ++i) {
      auto sub_data = static_cast<rmw_subscription_data_t *>(subscriptions->subscribers[i]);
      if (sub_data != nullptr) {
        sub_data->condition = &wait_set_data->condition_variable;
      }
    }
  }

  std::unique_lock<std::mutex> lock(wait_set_data->condition_mutex);

  // According to the RMW documentation, if wait_timeout is NULL that means
  // "wait forever", if it specified by 0 it means "never wait", and if it is anything else wait
  // for that amount of time.
  if (wait_timeout == nullptr) {
    wait_set_data->condition_variable.wait(lock);
  } else {
    if (wait_timeout->sec != 0 || wait_timeout->nsec != 0) {
      wait_set_data->condition_variable.wait_for(
        lock, std::chrono::nanoseconds(wait_timeout->nsec + RCUTILS_S_TO_NS(wait_timeout->sec)));
    }
  }

  if (guard_conditions) {
    // Now detach the condition variable and mutex from each of the guard conditions
    for (size_t i = 0; i < guard_conditions->guard_condition_count; ++i) {
      GuardCondition * gc = static_cast<GuardCondition *>(guard_conditions->guard_conditions[i]);
      if (gc != nullptr) {
        gc->detach_condition();
        // According to the documentation for rmw_wait in rmw.h, entries in the
        // array that have *not* been triggered should be set to NULL
        if (!gc->has_triggered()) {
          guard_conditions->guard_conditions[i] = nullptr;
        }
      }
    }
  }

  if (subscriptions) {
    // Now detach the condition variable and mutex from each of the subscriptions
    for (size_t i = 0; i < subscriptions->subscriber_count; ++i) {
      auto sub_data = static_cast<rmw_subscription_data_t *>(subscriptions->subscribers[i]);
      if (sub_data != nullptr) {
        sub_data->condition = nullptr;
        // According to the documentation for rmw_wait in rmw.h, entries in the
        // array that have *not* been triggered should be set to NULL
        if (sub_data->message_queue.empty()) {
          // Setting to nullptr lets rcl know that this subscription is not ready
          subscriptions->subscribers[i] = nullptr;
        }
      }
    }
  }

  return RMW_RET_OK;
}

//==============================================================================
/// Return the name and namespace of all nodes in the ROS graph.
rmw_ret_t
rmw_get_node_names(
  const rmw_node_t * node,
  rcutils_string_array_t * node_names,
  rcutils_string_array_t * node_namespaces)
{
  static_cast<void>(node);
  static_cast<void>(node_names);
  static_cast<void>(node_namespaces);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Return the name, namespae, and enclave name of all nodes in the ROS graph.
rmw_ret_t
rmw_get_node_names_with_enclaves(
  const rmw_node_t * node,
  rcutils_string_array_t * node_names,
  rcutils_string_array_t * node_namespaces,
  rcutils_string_array_t * enclaves)
{
  static_cast<void>(node);
  static_cast<void>(node_names);
  static_cast<void>(node_namespaces);
  static_cast<void>(enclaves);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Count the number of known publishers matching a topic name.
rmw_ret_t
rmw_count_publishers(
  const rmw_node_t * node,
  const char * topic_name,
  size_t * count)
{
  static_cast<void>(node);
  static_cast<void>(topic_name);
  static_cast<void>(count);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Count the number of known subscribers matching a topic name.
rmw_ret_t
rmw_count_subscribers(
  const rmw_node_t * node,
  const char * topic_name,
  size_t * count)
{
  static_cast<void>(node);
  static_cast<void>(topic_name);
  static_cast<void>(count);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Count the number of known clients matching a service name.
rmw_ret_t
rmw_count_clients(
  const rmw_node_t * node,
  const char * service_name,
  size_t * count)
{
  static_cast<void>(node);
  static_cast<void>(service_name);
  static_cast<void>(count);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Count the number of known servers matching a service name.
rmw_ret_t
rmw_count_services(
  const rmw_node_t * node,
  const char * service_name,
  size_t * count)
{
  static_cast<void>(node);
  static_cast<void>(service_name);
  static_cast<void>(count);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Get the globally unique identifier (GID) of a publisher.
rmw_ret_t
rmw_get_gid_for_publisher(const rmw_publisher_t * publisher, rmw_gid_t * gid)
{
  static_cast<void>(publisher);
  static_cast<void>(gid);
  // TODO(clalancette): Implement me
  return RMW_RET_OK;
}

//==============================================================================
/// Get the globally unique identifier (GID) of a service client.
rmw_ret_t
rmw_get_gid_for_client(const rmw_client_t * client, rmw_gid_t * gid)
{
  static_cast<void>(client);
  static_cast<void>(gid);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Check if two globally unique identifiers (GIDs) are equal.
rmw_ret_t
rmw_compare_gids_equal(const rmw_gid_t * gid1, const rmw_gid_t * gid2, bool * result)
{
  static_cast<void>(gid1);
  static_cast<void>(gid2);
  static_cast<void>(result);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Check if a service server is available for the given service client.
rmw_ret_t
rmw_service_server_is_available(
  const rmw_node_t * node,
  const rmw_client_t * client,
  bool * is_available)
{
  static_cast<void>(node);
  static_cast<void>(client);
  static_cast<void>(is_available);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Set the current log severity
rmw_ret_t
rmw_set_log_severity(rmw_log_severity_t severity)
{
  static_cast<void>(severity);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Set the on new message callback function for the subscription.
rmw_ret_t
rmw_subscription_set_on_new_message_callback(
  rmw_subscription_t * subscription,
  rmw_event_callback_t callback,
  const void * user_data)
{
  static_cast<void>(subscription);
  static_cast<void>(callback);
  static_cast<void>(user_data);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Set the on new request callback function for the service.
rmw_ret_t
rmw_service_set_on_new_request_callback(
  rmw_service_t * service,
  rmw_event_callback_t callback,
  const void * user_data)
{
  static_cast<void>(service);
  static_cast<void>(callback);
  static_cast<void>(user_data);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Set the on new response callback function for the client.
rmw_ret_t
rmw_client_set_on_new_response_callback(
  rmw_client_t * client,
  rmw_event_callback_t callback,
  const void * user_data)
{
  static_cast<void>(client);
  static_cast<void>(callback);
  static_cast<void>(user_data);
  return RMW_RET_UNSUPPORTED;
}

//==============================================================================
/// Set the callback function for the event.
rmw_ret_t
rmw_event_set_callback(
  rmw_event_t * event,
  rmw_event_callback_t callback,
  const void * user_data)
{
  static_cast<void>(event);
  static_cast<void>(callback);
  static_cast<void>(user_data);
  return RMW_RET_UNSUPPORTED;
}
}  // extern "C"
