// Copyright 2020 Open Source Robotics Foundation, Inc.
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

#ifndef IMPL__DEBUG_HELPERS_HPP_
#define IMPL__DEBUG_HELPERS_HPP_

#include "rmw/types.h"

namespace rmw_zenoh_common_cpp
{

void log_debug_qos_profile(const rmw_qos_profile_t * qos_profile);

}

#endif  // IMPL__DEBUG_HELPERS_HPP_
