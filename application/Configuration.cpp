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
#include "protocol/json/JsonDto.h"
#include "utilities/FileSystemUtils.h"
#include "utilities/json.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace wolkabout
{
using nlohmann::json;

const std::string GatewayConfiguration::KEY = "key";
const std::string GatewayConfiguration::PASSWORD = "password";
const std::string GatewayConfiguration::PLATFORM_URI = "platformMqttUri";
const std::string GatewayConfiguration::PLATFORM_TRUST_STORE = "platformTrustStore";
const std::string GatewayConfiguration::LOCAL_URI = "localMqttUri";
const std::string GatewayConfiguration::KEEP_ALIVE = "keepAlive";
const std::string GatewayConfiguration::SUBDEVICE_MANAGEMENT = "subdeviceManagement";

GatewayConfiguration::GatewayConfiguration(wolkabout::GatewayDevice device, std::string platformMqttUri,
                                           std::string localMqttUri, unsigned interval, ValueGenerator generator)
: m_device(std::move(device))
, m_platformMqttUri(std::move(platformMqttUri))
, m_localMqttUri(std::move(localMqttUri))
, m_interval(interval)
, m_valueGenerator(generator)
{
}

const wolkabout::GatewayDevice& GatewayConfiguration::getDevice() const
{
    return m_device;
}

const std::string& GatewayConfiguration::getLocalMqttUri() const
{
    return m_localMqttUri;
}

const std::string& GatewayConfiguration::getPlatformMqttUri() const
{
    return m_platformMqttUri;
}

unsigned GatewayConfiguration::getInterval() const
{
    return m_interval;
}

ValueGenerator GatewayConfiguration::getValueGenerator() const
{
    return m_valueGenerator;
}

void GatewayConfiguration::setPlatformTrustStore(const std::string& value)
{
    m_platformTrustStore = value;
}

const WolkOptional<std::string>& GatewayConfiguration::getPlatformTrustStore() const
{
    return m_platformTrustStore;
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
    const auto platformMqttUri = j.at(PLATFORM_URI).get<std::string>();
    const auto localMqttUri = j.at(LOCAL_URI).get<std::string>();
    auto subdeviceManagement = j.at(SUBDEVICE_MANAGEMENT).get<std::string>();

    std::transform(subdeviceManagement.begin(), subdeviceManagement.end(), subdeviceManagement.begin(), ::toupper);

    SubdeviceManagement management;

    if (subdeviceManagement == "PLATFORM")
    {
        management = SubdeviceManagement::PLATFORM;
    }
    else if (subdeviceManagement == "GATEWAY")
    {
        management = SubdeviceManagement::GATEWAY;
    }
    else
    {
        throw std::logic_error("Invalid value for subdevice management.");
    }

    wolkabout::DeviceTemplate manifest = j.at("manifest").get<wolkabout::DeviceTemplate>();

    manifest.addTypeParameter({"subdeviceManagement", static_cast<bool>(management) ? "GATEWAY" : "PLATFORM"});

    unsigned interval = 1000;
    if (j.find("readingsInterval") != j.end())
    {
        interval = j.at("readingsInterval").get<unsigned>();
    }

    ValueGenerator valueGenerator = ValueGenerator::RANDOM;
    if (j.find("generator") != j.end())
    {
        const auto gen = j.at("generator").get<std::string>();
        if (gen == "incremental")
        {
            valueGenerator = ValueGenerator::INCEREMENTAL;
        }
    }

    wolkabout::GatewayDevice device{key, password, manifest};

    GatewayConfiguration configuration(device, platformMqttUri, localMqttUri, interval, valueGenerator);

    if (j.find(KEEP_ALIVE) != j.end())
    {
        configuration.setKeepAliveEnabled(j.at(KEEP_ALIVE).get<bool>());
    }

    if (j.find(PLATFORM_TRUST_STORE) != j.end())
    {
        configuration.setPlatformTrustStore(j.at(PLATFORM_TRUST_STORE).get<std::string>());
    }

    return configuration;
}
}    // namespace wolkabout
