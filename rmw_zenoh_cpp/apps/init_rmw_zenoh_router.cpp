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

#include <stdlib.h>

#include <iostream>
#include <string>

#include <ament_index_cpp/get_package_share_directory.hpp>

namespace rmw_zenoh_cpp
{

int Main(int, char **)
{
  static const char * RMW_ZENOH_IDENTIFIER = "rmw_zenoh_cpp";
  static const char * ZENOH_ROUTER_CONFIG_NAME = "DEFAULT_RMW_ZENOH_ROUTER_CONFIG.json5";
  const std::string zenoh_router_config_path =
    ament_index_cpp::get_package_share_directory(RMW_ZENOH_IDENTIFIER) +
    "/config/" + std::string(ZENOH_ROUTER_CONFIG_NAME);

  // Execute zenohd command
  const std::string zenohd_cmd = "zenohd -c " + zenoh_router_config_path;
  const int ret = system(zenohd_cmd.c_str());
  if (ret != 0) {
    std::cerr << "Failed to run zenoh router: " << zenohd_cmd << std::endl;
    return ret;
  }
  return 0;
}

}  // namespace rmw_zenoh_cpp

int main(int argc, char ** argv) {return rmw_zenoh_cpp::Main(argc, argv);}
