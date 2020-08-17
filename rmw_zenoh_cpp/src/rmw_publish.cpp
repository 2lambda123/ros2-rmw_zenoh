#include <fastcdr/FastBuffer.h>
#include <fastcdr/Cdr.h>

#include "rcutils/logging_macros.h"

#include <rmw/validate_full_topic_name.h>
#include "rmw/impl/cpp/macros.hpp"
#include "rmw/error_handling.h"
#include "rmw/event.h"
#include "rmw/rmw.h"

#include "rmw_zenoh_cpp/identifier.hpp"

#include "type_support_common.hpp"
#include "pubsub_impl.hpp"

extern "C"
{
#include "zenoh/zenoh-ffi.h"

/// PUBLISH ROS MESSAGE ========================================================
// Serialize and publish a ROS message using Zenoh.
rmw_ret_t
rmw_publish(
  const rmw_publisher_t * publisher,
  const void * ros_message,
  rmw_publisher_allocation_t * allocation)
{
  (void) allocation;
  // RCUTILS_LOG_INFO_NAMED("rmw_zenoh_cpp",
  //                        "rmw_publish to %s, %ld",
  //                        publisher->topic_name,
  //                        static_cast<rmw_publisher_data_t *>(publisher->data)->zn_topic_id_);

  // ASSERTIONS ================================================================
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);

  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher,
    publisher->implementation_identifier,
    eclipse_zenoh_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION
  );

  auto publisher_data = static_cast<rmw_publisher_data_t *>(publisher->data);
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher_data, RMW_RET_ERROR);

  // ASSIGN ALLOCATOR ==========================================================
  rcutils_allocator_t * allocator =
    &static_cast<rmw_publisher_data_t *>(publisher->data)->node_->context->options.allocator;

  // SERIALIZE DATA ============================================================
  size_t max_data_length = (
    static_cast<rmw_publisher_data_t *>(publisher->data)->type_support_->getEstimatedSerializedSize(ros_message)
  );

  // Init serialized message byte array
  char * msg_bytes = static_cast<char *>(
    allocator->allocate(max_data_length, allocator->state)
  );

  eprosima::fastcdr::FastBuffer fastbuffer(
    msg_bytes,
    max_data_length
  );  // Object that manages the raw buffer.

  eprosima::fastcdr::Cdr ser(fastbuffer,
                             eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
                             eprosima::fastcdr::Cdr::DDS_CDR);  // Object that serializes the data.
  if (!publisher_data->type_support_->serializeROSmessage(const_cast<void *>(ros_message),
                                                         ser,
                                                         publisher_data->type_support_impl_)) {
    allocator->deallocate(msg_bytes, allocator->state);
    return RMW_RET_ERROR;
  }

  size_t data_length = ser.getSerializedDataLength();

  // // PUBLISH ON ZENOH MIDDLEWARE LAYER =========================================
  zn_write_wrid(publisher_data->zn_session_,
                publisher_data->zn_topic_id_,
                msg_bytes,
                data_length);

  allocator->deallocate(msg_bytes, allocator->state);
  return RMW_RET_OK;
}

rmw_ret_t
rmw_publish_serialized_message(
  const rmw_publisher_t * publisher,
  const rmw_serialized_message_t * serialized_message,
  rmw_publisher_allocation_t * allocation)
{
  (void)publisher;
  (void)serialized_message;
  (void)allocation;
  RCUTILS_LOG_INFO_NAMED("rmw_zenoh_cpp", "rmw_publish_serialized_message");
  return RMW_RET_ERROR;
}

rmw_ret_t
rmw_publish_loaned_message(
  const rmw_publisher_t * publisher,
  void * ros_message,
  rmw_publisher_allocation_t * allocation)
{
  (void)publisher;
  (void)ros_message;
  (void)allocation;
  RCUTILS_LOG_INFO_NAMED("rmw_zenoh_cpp", "rmw_publish_loaned_message");
  return RMW_RET_ERROR;
}

} // extern "C"
