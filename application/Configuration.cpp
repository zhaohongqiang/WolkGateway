/*
 * Copyright 2018 WolkAbout Technology s.r.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Configuration.h"
#include "utilities/FileSystemUtils.h"
#include "utilities/json.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace wolkabout
{
using nlohmann::json;

const std::string GatewayConfiguration::KEY = "key";
const std::string GatewayConfiguration::PASSWORD = "password";
const std::string GatewayConfiguration::PROTOCOL = "protocol";
const std::string GatewayConfiguration::PLATFORM_URI = "platformMqttUri";
const std::string GatewayConfiguration::LOCAL_URI = "localMqttUri";
const std::string GatewayConfiguration::KEEP_ALIVE = "keepAlive";

GatewayConfiguration::GatewayConfiguration(std::string key, std::string password, std::string protocol,
                                           std::string platformMqttUri, std::string localMqttUri)
: m_key(std::move(key))
, m_password(std::move(password))
, m_protocol(std::move(protocol))
, m_platformMqttUri(std::move(platformMqttUri))
, m_localMqttUri(std::move(localMqttUri))
{
}

const std::string& GatewayConfiguration::getKey() const
{
    return m_key;
}

const std::string& GatewayConfiguration::getPassword() const
{
    return m_password;
}

const std::string& GatewayConfiguration::getProtocol() const
{
    return m_protocol;
}

const std::string& GatewayConfiguration::getLocalMqttUri() const
{
    return m_localMqttUri;
}

const std::string& GatewayConfiguration::getPlatformMqttUri() const
{
    return m_platformMqttUri;
}

void GatewayConfiguration::setKeepAliveEnabled(bool value)
{
    m_keepAliveEnabled = value;
}

const WolkOptional<bool>& GatewayConfiguration::getKeepAliveEnabled() const
{
    return m_keepAliveEnabled;
}

wolkabout::GatewayConfiguration GatewayConfiguration::fromJson(const std::string& gatewayConfigurationFile)
{
    if (!FileSystemUtils::isFilePresent(gatewayConfigurationFile))
    {
        throw std::logic_error("Given gateway configuration file does not exist.");
    }

    std::string gatewayConfigurationJson;
    if (!FileSystemUtils::readFileContent(gatewayConfigurationFile, gatewayConfigurationJson))
    {
        throw std::logic_error("Unable to read gateway configuration file.");
    }

    auto j = json::parse(gatewayConfigurationJson);
    const auto key = j.at(KEY).get<std::string>();
    const auto password = j.at(PASSWORD).get<std::string>();
    const auto protocol = j.at(PROTOCOL).get<std::string>();
    const auto platformMqttUri = j.at(PLATFORM_URI).get<std::string>();
    const auto localMqttUri = j.at(LOCAL_URI).get<std::string>();

    GatewayConfiguration configuration(key, password, protocol, platformMqttUri, localMqttUri);

    if (j.find(KEEP_ALIVE) != j.end())
    {
        configuration.setKeepAliveEnabled(j.at(KEEP_ALIVE).get<bool>());
    }

    return configuration;
}
}    // namespace wolkabout